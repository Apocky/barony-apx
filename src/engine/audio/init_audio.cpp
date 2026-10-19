/*-------------------------------------------------------------------------------

	BARONY
	File: init_audio.cpp
	Desc: init, load, exit audio engine stuff.

	Copyright 2013-2016 (c) Turning Wheel LLC, all rights reserved.
	See LICENSE for details.

-------------------------------------------------------------------------------*/

#include "../../main.hpp"
#include "../../files.hpp"
#include "../../ui/LoadingScreen.hpp"
#include "sound.hpp"
#include <algorithm>
#include <mutex>
#include <atomic>
#include <future>
#include <thread>

#ifndef EDITOR
#include "../../ui/MainMenu.hpp"
#endif

#ifdef USE_FMOD

FMOD_SPEAKERMODE fmod_speakermode = FMOD_SPEAKERMODE_DEFAULT;
const char* fmod_speakermode_strings[FMOD_SPEAKERMODE_MAX] = {
    "FMOD_SPEAKERMODE_DEFAULT",
    "FMOD_SPEAKERMODE_RAW",
    "FMOD_SPEAKERMODE_MONO",
    "FMOD_SPEAKERMODE_STEREO",
    "FMOD_SPEAKERMODE_QUAD",
    "FMOD_SPEAKERMODE_SURROUND",
    "FMOD_SPEAKERMODE_5POINT1",
    "FMOD_SPEAKERMODE_7POINT1",
    "FMOD_SPEAKERMODE_7POINT1POINT4",
};

#endif

bool initSoundEngine()
{
#ifdef USE_FMOD
	printlog("[FMOD]: initializing FMOD...\n");
	fmod_result = FMOD::System_Create(&fmod_system);
	if (FMODErrorCheck())
	{
		printlog("[FMOD]: Failed to create FMOD. DISABLING AUDIO.\n");
		no_sound = true;
		return false;
	}
 
    int numRawSpeakers{};
    int sampleRate{};
    FMOD_SPEAKERMODE speakerMode{};
    fmod_system->getSoftwareFormat(&sampleRate, &speakerMode, &numRawSpeakers);
    fmod_system->setSoftwareFormat(sampleRate, fmod_speakermode, numRawSpeakers);
	FMOD_ADVANCEDSETTINGS settings{};
	settings.cbSize = sizeof(FMOD_ADVANCEDSETTINGS);
	settings.vol0virtualvol = 0.001;
	fmod_system->setAdvancedSettings(&settings);

	// default 64
	fmod_system->setSoftwareChannels(32);

	if (!no_sound)
	{
		FMOD_INITFLAGS flags = FMOD_INIT_NORMAL | FMOD_INIT_3D_RIGHTHANDED | FMOD_INIT_VOL0_BECOMES_VIRTUAL | FMOD_INIT_STREAM_FROM_UPDATE | FMOD_INIT_THREAD_UNSAFE;
#ifndef NDEBUG
		flags |= FMOD_INIT_PROFILE_ENABLE | FMOD_INIT_PROFILE_METER_ALL;
#endif
		fmod_result = fmod_system->init(fmod_maxchannels, flags, fmod_extraDriverData);
		if (FMODErrorCheck())
		{
			printlog("[FMOD]: Failed to initialize FMOD. DISABLING AUDIO.\n");
			no_sound = true;
			return false;
		}

#ifndef NDEBUG
		//FMOD::Debug_Initialize(FMOD_DEBUG_LEVEL_WARNING | FMOD_DEBUG_TYPE_MEMORY);
#endif

		int selected_driver = 0;
		int numDrivers = 0;
		fmod_system->getNumDrivers(&numDrivers);
		for ( int i = 0; i < numDrivers; ++i )
		{
            constexpr int driverNameLen = 64;
            char driverName[driverNameLen] = "";
            FMOD_GUID guid;
            int rate{}, channels{};
            FMOD_SPEAKERMODE mode{};
            fmod_result = fmod_system->getDriverInfo(i, driverName, driverNameLen, &guid, &rate, &mode, &channels);
			if ( FMODErrorCheck() )
			{
				printlog("[FMOD]: Failed to read audio device index: %d", i);
			}
        
            mode = (FMOD_SPEAKERMODE)std::clamp((int)mode, (int)0, (int)FMOD_SPEAKERMODE_MAX - 1);
            printlog("[FMOD] Audio device found: %d %s | %08x %04x %04x | rate: %d | mode: %s | channels: %d",
                i, driverName, guid.Data1, guid.Data2, guid.Data3, rate, fmod_speakermode_strings[mode], channels);

#ifndef EDITOR
			uint32_t _1; memcpy(&_1, &guid.Data1, sizeof(_1));
			uint64_t _2; memcpy(&_2, &guid.Data4, sizeof(_2));
			char guid_string[25];
			snprintf(guid_string, sizeof(guid_string), FMOD_AUDIO_GUID_FMT, _1, _2);
			if (!selected_driver && MainMenu::current_audio_device == guid_string)
			{
				selected_driver = i;
			}
#endif
		}

		fmod_system->setDriver(selected_driver);
		fmod_system->getDriver(&selected_driver);
		printlog("[FMOD]: Current audio device: %d", selected_driver);

		fmod_result = fmod_system->createChannelGroup(nullptr, &sound_group);
		if (FMODErrorCheck())
		{
			printlog("[FMOD]: Failed to create sound channel group. DISABLING AUDIO.\n");
			no_sound = true;
			return false;
		}
		fmod_result = fmod_system->createChannelGroup(nullptr, &soundAmbient_group);
		if ( FMODErrorCheck() )
		{
			printlog("F[FMOD]: ailed to create sound ambient channel group.\n");
			no_sound = true;
		}
		fmod_result = fmod_system->createChannelGroup(nullptr, &soundEnvironment_group);
		if ( FMODErrorCheck() )
		{
			printlog("[FMOD]: Failed to create sound environment channel group.\n");
			no_sound = true;
		}
		fmod_result = fmod_system->createChannelGroup(nullptr, &music_notification_group);
		if ( FMODErrorCheck() )
		{
			printlog("[FMOD]: Failed to create notification channel group.\n");
			no_sound = true;
		}
		fmod_result = fmod_system->createChannelGroup(nullptr, &soundNotification_group);
		if ( FMODErrorCheck() )
		{
			printlog("[FMOD]: Failed to create sound notification channel group.\n");
			no_sound = true;
		}
		fmod_result = fmod_system->createChannelGroup(nullptr, &music_group);
		if (FMODErrorCheck())
		{
			printlog("[FMOD]: Failed to create music channel group. DISABLING AUDIO.\n");
			no_sound = true;
			return false;
		}
		fmod_result = fmod_system->createChannelGroup(nullptr, &music_ensemble_global_send_group);
		if ( FMODErrorCheck() )
		{
			printlog("[FMOD]: Failed to create music channel group. DISABLING AUDIO.\n");
			no_sound = true;
			return false;
		}
		music_ensemble_global_send_group->setVolumeRamp(true);

		fmod_result = fmod_system->createChannelGroup(nullptr, &music_ensemble_global_recv_group);
		if ( FMODErrorCheck() )
		{
			printlog("[FMOD]: Failed to create music channel group. DISABLING AUDIO.\n");
			no_sound = true;
			return false;
		}

		fmod_result = fmod_system->createChannelGroup(nullptr, &music_ensemble_local_recv_group);
		if ( FMODErrorCheck() )
		{
			printlog("[FMOD]: Failed to create music channel group. DISABLING AUDIO.\n");
			no_sound = true;
			return false;
		}
		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			fmod_result = fmod_system->createChannelGroup(nullptr, &music_ensemble_local_recv_player[i]);
			if ( FMODErrorCheck() )
			{
				printlog("[FMOD]: Failed to create music channel group. DISABLING AUDIO.\n");
				no_sound = true;
				return false;
			}
			music_ensemble_local_recv_group->addGroup(music_ensemble_local_recv_player[i]);
			music_ensemble_local_recv_player[i]->setMode(FMOD_3D | FMOD_3D_WORLDRELATIVE);
		}
		{
			// add dsp

			//FMOD::DSP* dspeq = 0;
			//fmod_result = fmod_system->createDSPByType(FMOD_DSP_TYPE_MULTIBAND_EQ, &dspeq);
			//FMODErrorCheck();
			//
			//fmod_result = music_ensemble_global_group->addDSP(0, dspeq);
			//FMODErrorCheck();
			//
			//dspeq->setParameterInt(FMOD_DSP_MULTIBAND_EQ_A_FILTER, FMOD_DSP_MULTIBAND_EQ_FILTER_NOTCH);
			//dspeq->setParameterFloat(FMOD_DSP_MULTIBAND_EQ_A_FREQUENCY, 2000);
			//dspeq->setParameterFloat(FMOD_DSP_MULTIBAND_EQ_A_GAIN, -6);
			//dspeq->setParameterFloat(FMOD_DSP_MULTIBAND_EQ_A_Q, 1.f);

			// global sends
			{
				// send group does not output to master bus
				FMOD::DSP* fader = nullptr;
				fmod_result = fmod_system->createDSPByType(FMOD_DSP_TYPE_FADER, &fader);
				fader->setParameterFloat(FMOD_DSP_FADER_GAIN, -80.f); // inaudible
				music_ensemble_global_send_group->addDSP(0, fader);
			}

			// global recv transceivers
			{
				for ( int i = 0; i < NUMENSEMBLEMUSIC; ++i )
				{
					FMOD::DSP* transceiver = nullptr;
					fmod_result = fmod_system->createDSPByType(FMOD_DSP_TYPE_TRANSCEIVER, &transceiver);
					music_ensemble_global_recv_group->addDSP(1, transceiver);
					transceiver->setParameterInt(FMOD_DSP_TRANSCEIVER_CHANNEL, i + 1); // receive on channel x
					transceiver->setParameterFloat(FMOD_DSP_TRANSCEIVER_GAIN, -80.f); // inaudible
					transceiver->setChannelFormat(0, 2, FMOD_SPEAKERMODE_STEREO); // force stereo on empty channel, otherwise defaults to mono
				}
			}

			// player recv transceivers
			{
				for ( int c = 0; c < MAXPLAYERS; ++c )
				{
					for ( int i = 0; i < NUMENSEMBLEMUSIC; ++i )
					{
						FMOD::DSP* transceiver = nullptr;
						fmod_result = fmod_system->createDSPByType(FMOD_DSP_TYPE_TRANSCEIVER, &transceiver);
						music_ensemble_local_recv_player[c]->addDSP(1, transceiver);
						transceiver->setParameterInt(FMOD_DSP_TRANSCEIVER_CHANNEL, i + 1); // receive on channel x
						transceiver->setParameterFloat(FMOD_DSP_TRANSCEIVER_GAIN, -80.f); // inaudible
						transceiver->setChannelFormat(0, 2, FMOD_SPEAKERMODE_STEREO); // force stereo on empty channel, otherwise defaults to mono
					}
				}
			}

			// global recv reverb
			{
				FMOD::DSP* dspreverb = 0;
				fmod_result = fmod_system->createDSPByType(FMOD_DSP_TYPE_SFXREVERB, &dspreverb);
				FMODErrorCheck();

				fmod_result = music_ensemble_global_recv_group->addDSP(0, dspreverb);
				dspreverb->setBypass(true);
				FMODErrorCheck();

				FMOD_REVERB_PROPERTIES props = FMOD_PRESET_OFF;
				dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_DECAYTIME, props.DecayTime);
				dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_EARLYDELAY, props.EarlyDelay);
				dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_LATEDELAY, props.LateDelay);
				dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_HFREFERENCE, props.HFReference);
				dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_HFDECAYRATIO, props.HFDecayRatio);
				dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_DIFFUSION, props.Diffusion);
				dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_DENSITY, props.Density);
				dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_LOWSHELFFREQUENCY, props.LowShelfFrequency);
				dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_LOWSHELFGAIN, props.LowShelfGain);
				dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_HIGHCUT, props.HighCut);
				dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_EARLYLATEMIX, props.EarlyLateMix);
				dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_WETLEVEL, props.WetLevel);
				dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_DRYLEVEL, 0.f);
			}
		}
		//{
		//	// add dsp

		//	//FMOD::DSP* dspeq = 0;
		//	//fmod_result = fmod_system->createDSPByType(FMOD_DSP_TYPE_MULTIBAND_EQ, &dspeq);
		//	//FMODErrorCheck();
		//	//
		//	//fmod_result = music_ensemble_local_group->addDSP(0, dspeq);
		//	//FMODErrorCheck();
		//	//
		//	//dspeq->setParameterInt(FMOD_DSP_MULTIBAND_EQ_A_FILTER, FMOD_DSP_MULTIBAND_EQ_FILTER_NOTCH);
		//	//dspeq->setParameterFloat(FMOD_DSP_MULTIBAND_EQ_A_FREQUENCY, 2000);
		//	//dspeq->setParameterFloat(FMOD_DSP_MULTIBAND_EQ_A_GAIN, -6);
		//	//dspeq->setParameterFloat(FMOD_DSP_MULTIBAND_EQ_A_Q, 1.f);

		//	FMOD::DSP* dspreverb = 0;
		//	fmod_result = fmod_system->createDSPByType(FMOD_DSP_TYPE_SFXREVERB, &dspreverb);
		//	FMODErrorCheck();

		//	fmod_result = music_ensemble_local_group->addDSP(0, dspreverb);
		//	FMODErrorCheck();

		//	FMOD_REVERB_PROPERTIES props = FMOD_PRESET_OFF;
		//	dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_DECAYTIME, props.DecayTime);
		//	dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_EARLYDELAY, props.EarlyDelay);
		//	dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_LATEDELAY, props.LateDelay);
		//	dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_HFREFERENCE, props.HFReference);
		//	dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_HFDECAYRATIO, props.HFDecayRatio);
		//	dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_DIFFUSION, props.Diffusion);
		//	dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_DENSITY, props.Density);
		//	dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_LOWSHELFFREQUENCY, props.LowShelfFrequency);
		//	dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_LOWSHELFGAIN, props.LowShelfGain);
		//	dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_HIGHCUT, props.HighCut);
		//	dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_EARLYLATEMIX, props.EarlyLateMix);
		//	dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_WETLEVEL, props.WetLevel);
		//	dspreverb->setParameterFloat(FMOD_DSP_SFXREVERB_DRYLEVEL, 0.f);
		//}

#ifndef EDITOR
		int selected_recording_driver = 0;
		int numRecordingDrivers = 0;
		fmod_system->getRecordNumDrivers(&numRecordingDrivers, nullptr);
		for ( int i = 0; i < numRecordingDrivers; ++i )
		{
			constexpr int driverNameLen = 64;
			char driverName[driverNameLen] = "";
			FMOD_GUID guid;
			int rate{}, channels{};
			FMOD_SPEAKERMODE mode{};
			FMOD_DRIVER_STATE state{};
			fmod_result = fmod_system->getRecordDriverInfo(i, driverName, driverNameLen, &guid, &rate, &mode, &channels, &state);
			if ( FMODErrorCheck() )
			{
				printlog("[FMOD]: Failed to read recording device index: %d", i);
			}
			if ( strstr(driverName, "[loopback]") )
			{
				continue;
			}

			mode = (FMOD_SPEAKERMODE)std::clamp((int)mode, (int)0, (int)FMOD_SPEAKERMODE_MAX - 1);
			printlog("[FMOD] Recording device found: %d %s | %08x %04x %04x | rate: %d | mode: %s | channels: %d",
				i, driverName, guid.Data1, guid.Data2, guid.Data3, rate, fmod_speakermode_strings[mode], channels);

			uint32_t _1; memcpy(&_1, &guid.Data1, sizeof(_1));
			uint64_t _2; memcpy(&_2, &guid.Data4, sizeof(_2));
			char guid_string[25];
			snprintf(guid_string, sizeof(guid_string), FMOD_AUDIO_GUID_FMT, _1, _2);
			if ( !selected_recording_driver && MainMenu::current_recording_audio_device == ""
				&& (state & FMOD_DRIVER_STATE_DEFAULT))
			{
				selected_recording_driver = i;
			}
			else if ( !selected_recording_driver && MainMenu::current_recording_audio_device == guid_string )
			{
				selected_recording_driver = i;
			}
		}
#endif

#ifndef EDITOR
		VoiceChat.setRecordingDevice(selected_recording_driver);
#endif
	}
#elif defined USE_OPENAL
	if (!no_sound)
	{
		initOPENAL();
	}
#endif

#ifndef EDITOR
	// saves your ears getting blasted if the game starts without window focus.
	setGlobalVolume(0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
#endif

	return !no_sound; //No double negatives pls
}

#ifdef USE_FMOD
// apx (BL-load fixes 3 and 4): FMOD decodes every sound effect (860 .ogg) and every ensemble music file
// into memory on one thread -- sound effects synchronously (~3 s), ensemble music on FMOD's single
// non-blocking loader thread (~6 s, waited for in EnsembleSounds_t::setup). The FMOD Core API is
// thread-safe, so both now decode across worker threads with plain synchronous createSound calls.
// Measured and rejected (2026-10-19): FMOD_NONBLOCKING for sound effects queues them ahead of the music
// on that one loader thread (floor 1 later by 2.3 s); FMOD_CREATECOMPRESSEDSAMPLE gains nothing for .ogg.
// Controls: BARONY_APX_SOUNDS=sync (old serial loop), BARONY_APX_ENSEMBLE=nonblocking (old loader thread).
static bool apxEnvIs(const char* name, const char* value)
{
	const char* env = getenv(name);
	return env && !strcmp(env, value);
}

unsigned apxSoundThreads()
{
	const unsigned hw = std::thread::hardware_concurrency();
	return std::max(2u, std::min(8u, hw > 1 ? hw - 1 : 2u));
}

struct ApxSoundJob
{
	std::string path;
	FMOD_MODE flags;
	FMOD::Sound** out;
	int line;
};

// creates every job's sound on worker threads; returns the number that failed
static int apxCreateSoundsParallel(std::vector<ApxSoundJob>& jobs, const char* what)
{
	std::atomic<size_t> next{ 0 };
	std::atomic<int> failed{ 0 };
	std::mutex logMutex;
	auto worker = [&]() {
		for ( size_t k = next++; k < jobs.size(); k = next++ )
		{
			FMOD::Sound* sound = nullptr;
			const FMOD_RESULT result = fmod_system->createSound(jobs[k].path.c_str(), jobs[k].flags, nullptr, &sound);
			*jobs[k].out = sound;
			if ( result != FMOD_OK )
			{
				++failed;
				std::lock_guard<std::mutex> lock(logMutex);
				printlog("warning: failed to load '%s' listed at line %d in %s (FMOD error %d)\n", jobs[k].path.c_str(), jobs[k].line, what, (int)result);
			}
		}
	};
	std::vector<std::thread> threads;
	const unsigned n = std::min<unsigned>(apxSoundThreads(), (unsigned)std::max<size_t>(1, jobs.size()));
	for ( unsigned t = 0; t < n; ++t ) { threads.emplace_back(worker); }
	for ( auto& t : threads ) { t.join(); }
	return failed;
}

// ensemble music: queued while physfsReloadMusic walks the music list, decoded in the background, and
// joined in EnsembleSounds_t::setup. Results land in private slots and are copied out only at the join,
// so nothing reads a half-made pointer meanwhile.
static std::vector<ApxSoundJob> apxEnsembleQueued;
static std::vector<ApxSoundJob> apxEnsembleRunning;
static std::vector<FMOD::Sound*> apxEnsembleResults;
static std::vector<FMOD::Sound**> apxEnsembleTargets;
static std::future<int> apxEnsembleFuture;
static Uint32 apxEnsembleStartTick = 0;

bool apxEnsembleParallel()
{
	return !apxEnvIs("BARONY_APX_ENSEMBLE", "nonblocking");
}

void apxQueueEnsembleSound(const std::string& path, FMOD_MODE flags, FMOD::Sound** out, int index)
{
	*out = nullptr; // the old sound was released by the caller; the new one is copied in at the join
	apxEnsembleQueued.push_back(ApxSoundJob{ path, flags & ~FMOD_NONBLOCKING, out, index });
}

void apxJoinEnsembleLoads()
{
	if ( !apxEnsembleFuture.valid() ) { return; }
	const Uint32 waitStart = SDL_GetTicks();
	const int failed = apxEnsembleFuture.get();
	for ( size_t k = 0; k < apxEnsembleTargets.size(); ++k ) { *apxEnsembleTargets[k] = apxEnsembleResults[k]; }
	printlog("[apx] ensemble music: %d files decoded on %u threads in %u ms, waited %u ms at the join, %d errors",
		(int)apxEnsembleRunning.size(), apxSoundThreads(), SDL_GetTicks() - apxEnsembleStartTick, SDL_GetTicks() - waitStart, failed);
	apxEnsembleRunning.clear();
	apxEnsembleResults.clear();
	apxEnsembleTargets.clear();
}

void apxStartEnsembleLoads()
{
	if ( apxEnsembleQueued.empty() ) { return; }
	apxJoinEnsembleLoads(); // a previous batch must land before its slots are reused
	apxEnsembleRunning = std::move(apxEnsembleQueued);
	apxEnsembleQueued.clear();
	apxEnsembleResults.assign(apxEnsembleRunning.size(), nullptr);
	apxEnsembleTargets.clear();
	for ( size_t k = 0; k < apxEnsembleRunning.size(); ++k )
	{
		apxEnsembleTargets.push_back(apxEnsembleRunning[k].out);
		apxEnsembleRunning[k].out = &apxEnsembleResults[k];
	}
	apxEnsembleStartTick = SDL_GetTicks();
	apxEnsembleFuture = std::async(std::launch::async, []() { return apxCreateSoundsParallel(apxEnsembleRunning, "the ensemble music list"); });
}
#endif

int loadSoundResources(real_t base_load_percent, real_t top_load_percent)
{
	File* fp;
	Uint32 c;
	char name[128];

	if ( !PHYSFS_getRealDir("sound/sounds.txt") )
	{
		printlog("error: could not find file: %s", "sound/sounds.txt");
		return 10;
	}

	// load sound effects
	std::string soundsDirectory = PHYSFS_getRealDir("sound/sounds.txt");
	soundsDirectory.append(PHYSFS_getDirSeparator()).append("sound/sounds.txt");
	printlog("loading sounds...\n");
	fp = openDataFile(soundsDirectory.c_str(), "rb");
	for ( numsounds = 0; !fp->eof(); ++numsounds )
	{
		while ( fp->getc() != '\n' )
		{
			if ( fp->eof() )
			{
				break;
			}
		}
	}
	FileIO::close(fp);
	if ( numsounds == 0 )
	{
		printlog("failed to identify any sounds in sounds.txt\n");
		return 10;
	}
#ifdef USE_FMOD
	sounds = (FMOD::Sound**) malloc(sizeof(FMOD::Sound*)*numsounds);
	memset(sounds, 0, sizeof(FMOD::Sound*) * numsounds);
	fp = openDataFile(soundsDirectory.c_str(), "rb");
	char full_path[PATH_MAX];
	const bool apxSerial = apxEnvIs("BARONY_APX_SOUNDS", "sync");
	const Uint32 apxSoundsStart = SDL_GetTicks();
	std::vector<ApxSoundJob> apxJobs;
	for ( c = 0; !fp->eof(); ++c )
	{
		fp->gets2(name, 128);
		completePath(full_path, name);
		FMOD_MODE flags = FMOD_DEFAULT | FMOD_3D | FMOD_LOWMEM;
		if ( c == 133 || c == 672 || c == 135 || c == 155 || c == 149 || c == 710 )
		{
			flags |= FMOD_LOOP_NORMAL;
		}
		if ( !apxSerial )
		{
			apxJobs.push_back(ApxSoundJob{ full_path, flags, &sounds[c], (int)c + 1 });
			continue;
		}
		fmod_result = fmod_system->createSound(full_path, flags, nullptr, &sounds[c]);
		if (FMODErrorCheck())
		{
			printlog("warning: failed to load '%s' listed at line %d in sounds.txt\n", full_path, c + 1);
		}
		updateLoadingScreen(base_load_percent + (top_load_percent * c) / numsounds);
	}
	FileIO::close(fp);
	if ( !apxSerial )
	{
		const int failed = apxCreateSoundsParallel(apxJobs, "sounds.txt");
		updateLoadingScreen(base_load_percent + top_load_percent);
		printlog("[apx] sounds: %d files on %u threads in %u ms, %d errors", (int)apxJobs.size(), apxSoundThreads(), SDL_GetTicks() - apxSoundsStart, failed);
	}
	else
	{
		printlog("[apx] sounds: serial (BARONY_APX_SOUNDS=sync) in %u ms", SDL_GetTicks() - apxSoundsStart);
	}
	fmod_system->set3DSettings(1.0, 2.0, 1.0);
#elif defined USE_OPENAL
	sounds = (OPENAL_BUFFER**) malloc(sizeof(OPENAL_BUFFER*)*numsounds);
	for (c = 0, fp = openDataFile(soundsDirectory.c_str(), "rb"); fp->gets2(name, 128); ++c)
	{
		//TODO: Might need to malloc the sounds[c]->sound
		OPENAL_CreateSound(name, true, &sounds[c]);
		//TODO: set sound volume? Or otherwise handle sound volume.
		updateLoadingScreen(base_load_percent + (top_load_percent * c) / numsounds);
	}
	FileIO::close(fp);
	//FMOD_System_Set3DSettings(fmod_system, 1.0, 2.0, 1.0); // This on is hardcoded, I've been lazy here'
#endif // defined USE_OPENAL

	return 0;
}

void freeSoundResources()
{
	uint32_t c;
	// free sounds
#ifdef USE_FMOD
	printlog("freeing sounds...\n");
	if ( sounds != nullptr )
	{
		for ( c = 0; c < numsounds && !no_sound; c++ )
		{
			if (sounds[c] != nullptr)
			{
				if (sounds[c] != nullptr)
				{
					sounds[c]->release(); //Free the sound's FMOD sound.
				}
			}
		}
		free(sounds); //Then free the sound array.
	}
#endif
}

void exitSoundEngine()
{
#ifdef USE_FMOD
	if ( fmod_system )
	{
// no idea why this causes the game to hang for me.
// someone else investigate? -skrathbun
#ifndef LINUX
		fmod_system->close();
		fmod_system->release();
#endif
		fmod_system = nullptr;
	}
#endif
}
