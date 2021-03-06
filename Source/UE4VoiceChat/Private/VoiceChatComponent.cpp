// Created by Kaan Buran
// See https://github.com/Naocum/UE4VoiceChat for documentation and licensing

#include "VoiceChatComponent.h"
#include "VoiceModule.h"
#include "Kismet/KismetSystemLibrary.h"
#include "AudioDeviceManager.h"
#include "Sound/SoundClass.h"

#define VOICE_BUFFER_CHECK(Buffer, Size) \
	check(Buffer.Num() >= (int32)(Size))

UVoiceChatComponent::UVoiceChatComponent() :
	SoundStreaming(nullptr),
	VoiceCapture(nullptr),
	VoiceEncoder(nullptr),
	VoiceDecoder(nullptr),
	EncodeHint(UVOIPStatics::GetAudioEncodingHint()),
	InputSampleRate(UVOIPStatics::GetVoiceSampleRate()),
	OutputSampleRate(UVOIPStatics::GetVoiceSampleRate()),
	NumInChannels(UVOIPStatics::GetVoiceNumChannels()),
	NumOutChannels(UVOIPStatics::GetVoiceNumChannels()),
	bLastWasPlaying(false),
	StarvedDataCount(0),
	MaxRawCaptureDataSize(0),
	MaxCompressedDataSize(0),
	MaxUncompressedDataSize(0),
	CurrentUncompressedDataQueueSize(0),
	MaxUncompressedDataQueueSize(0),
	MaxRemainderSize(0),
	LastRemainderSize(0),
	CachedSampleCount(0),
	bZeroInput(false),
	bUseDecompressed(true),
	bZeroOutput(false)
{
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.bCanEverTick = true;
}

bool UVoiceChatComponent::Init()
{
	DeviceName = TEXT("Line 1 (Virtual Audio Cable)");
	EncodeHint = EAudioEncodeHint::VoiceEncode_Audio;
	InputSampleRate = 48000;
	OutputSampleRate = 48000;
	NumInChannels = 2;
	NumOutChannels = 2;

	InitVoiceCapture();
	InitVoiceEncoder();
	InitVoiceDecoder();

	return true;
}

bool UVoiceChatComponent::InitWithInputDevice(FName InputDeviceName)
{
	DeviceName = InputDeviceName.ToString();
	EncodeHint = EAudioEncodeHint::VoiceEncode_Audio;
	InputSampleRate = 48000;
	OutputSampleRate = 48000;
	NumInChannels = 2;
	NumOutChannels = 2;
	UE_LOG(LogVoice, Log, TEXT("Initialization started"));
	UKismetSystemLibrary::PrintString(this, FString("Initialization started "), true, true, FLinearColor::Red, 0.f);

	InitVoiceCapture();
	UE_LOG(LogVoice, Log, TEXT("Init Voice Capture ended"));
	UKismetSystemLibrary::PrintString(this, FString("Init Voice Capture ended "), true, true, FLinearColor::Red, 0.f);

	InitVoiceEncoder();
	UE_LOG(LogVoice, Log, TEXT("Init Voice Encoder ended"));
	UKismetSystemLibrary::PrintString(this, FString("Init Voice Encoder ended "), true, true, FLinearColor::Red, 0.f);

	InitVoiceDecoder();
	UE_LOG(LogVoice, Log, TEXT("Init Voice Decoder ended"));
	UKismetSystemLibrary::PrintString(this, FString("Init Voice Decoder ended "), true, true, FLinearColor::Red, 0.f);


	USoundWaveProcedural* newSoundStreaming = NewObject<USoundWaveProcedural>();
	newSoundStreaming->SetSampleRate(OutputSampleRate);
	newSoundStreaming->NumChannels = NumOutChannels;
	newSoundStreaming->Duration = INDEFINITELY_LOOPING_DURATION;
	newSoundStreaming->SoundGroup = SOUNDGROUP_Voice;
	newSoundStreaming->bLooping = false;

	// Turn off async generation in old audio engine on mac.
#if PLATFORM_MAC
	if (!AudioDevice->IsAudioMixerEnabled())
	{
		newSoundStreaming->bCanProcessAsync = false;
	}
	else
#endif // #if PLATFORM_MAC
	{
		newSoundStreaming->bCanProcessAsync = true;
	}

	Sound = newSoundStreaming;
	bIsUISound = false;
	bAllowSpatialization = true;
	SetVolumeMultiplier(1.5f);

	const FSoftObjectPath VoiPSoundClassName = GetDefault<UAudioSettings>()->VoiPSoundClass;
	if (VoiPSoundClassName.IsValid())
	{
		SoundClassOverride = LoadObject<USoundClass>(nullptr, *VoiPSoundClassName.ToString());
	}

	return true;
}

void UVoiceChatComponent::InitVoiceCapture()
{
	ensure(!VoiceCapture.IsValid());
	VoiceCapture = FVoiceModule::Get().CreateVoiceCapture(DeviceName, InputSampleRate, NumInChannels);
	if (VoiceCapture.IsValid())
	{
		MaxRawCaptureDataSize = VoiceCapture->GetBufferSize();

		RawCaptureData.Empty(MaxRawCaptureDataSize);
		RawCaptureData.AddUninitialized(MaxRawCaptureDataSize);

		VoiceCapture->Start();
		UE_LOG(LogVoice, Log, TEXT("Voice Capture started"));
		UKismetSystemLibrary::PrintString(this, FString("Voice Capture started "), true, true, FLinearColor::Red, 0.f);
	}
}

void UVoiceChatComponent::InitVoiceEncoder()
{
	ensure(!VoiceEncoder.IsValid());
	VoiceEncoder = FVoiceModule::Get().CreateVoiceEncoder(InputSampleRate, NumInChannels, EncodeHint);
	if (VoiceEncoder.IsValid())
	{
		MaxRemainderSize = VOICE_STARTING_REMAINDER_SIZE;
		LastRemainderSize = 0;
		MaxCompressedDataSize = VOICE_MAX_COMPRESSED_BUFFER;

		CompressedData.Empty(MaxCompressedDataSize);
		CompressedData.AddUninitialized(MaxCompressedDataSize);

		Remainder.Empty(MaxRemainderSize);
		Remainder.AddUninitialized(MaxRemainderSize);

		UE_LOG(LogVoice, Log, TEXT("Voice Encoder started"));
		UKismetSystemLibrary::PrintString(this, FString("Voice Encoder started "), true, true, FLinearColor::Red, 0.f);
	}
}

void UVoiceChatComponent::InitVoiceDecoder()
{
	ensure(!VoiceDecoder.IsValid());
	VoiceDecoder = FVoiceModule::Get().CreateVoiceDecoder(OutputSampleRate, NumOutChannels);
	if (VoiceDecoder.IsValid())
	{
		// Approx 1 sec worth of data
		MaxUncompressedDataSize = NumOutChannels * OutputSampleRate * sizeof(uint16);

		UncompressedData.Empty(MaxUncompressedDataSize);
		UncompressedData.AddUninitialized(MaxUncompressedDataSize);

		MaxUncompressedDataQueueSize = MaxUncompressedDataSize * 5;
		{
			FScopeLock ScopeLock(&QueueLock);
			UncompressedDataQueue.Empty(MaxUncompressedDataQueueSize);
		}

		UE_LOG(LogVoice, Log, TEXT("Voice Decoder started"));
		UKismetSystemLibrary::PrintString(this, FString("Voice Decoder started "), true, true, FLinearColor::Red, 0.f);
	}
}

void UVoiceChatComponent::Shutdown()
{
	RawCaptureData.Empty();
	CompressedData.Empty();
	UncompressedData.Empty();
	Remainder.Empty();

	{
		FScopeLock ScopeLock(&QueueLock);
		UncompressedDataQueue.Empty();
	}

	CleanupVoice();
	CleanupAudioComponent();
}

void UVoiceChatComponent::CleanupVoice()
{
	if (VoiceCapture.IsValid())
	{
		VoiceCapture->Shutdown();
		VoiceCapture = nullptr;
	}

	VoiceEncoder = nullptr;
	VoiceDecoder = nullptr;
}

void UVoiceChatComponent::CleanupAudioComponent()
{
	Stop();

	SoundStreaming->OnSoundWaveProceduralUnderflow.Unbind();
	SoundStreaming = nullptr;

	bLastWasPlaying = false;
}

void UVoiceChatComponent::CleanupQueue()
{
	FScopeLock ScopeLock(&QueueLock);
	UncompressedDataQueue.Reset();
}

void UVoiceChatComponent::GenerateData(USoundWaveProcedural* InProceduralWave, int32 SamplesRequired)
{
	const int32 SampleSize = sizeof(uint16) * NumOutChannels;

	{
		FScopeLock ScopeLock(&QueueLock);
		CurrentUncompressedDataQueueSize = UncompressedDataQueue.Num();
		const int32 AvailableSamples = CurrentUncompressedDataQueueSize / SampleSize;
		if (AvailableSamples >= SamplesRequired)
		{
			InProceduralWave->QueueAudio(UncompressedDataQueue.GetData(), AvailableSamples * SampleSize);
			UncompressedDataQueue.RemoveAt(0, AvailableSamples * SampleSize, false);
			CurrentUncompressedDataQueueSize -= (AvailableSamples * SampleSize);
		}
	}
}

void UVoiceChatComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	QUICK_SCOPE_CYCLE_COUNTER(STAT_FTestVoice_Tick);

	UKismetSystemLibrary::PrintString(this, FString("Voicechat Ticking"), true, true, FLinearColor::Red, 0.f);

	if (!IsRunningDedicatedServer() && IsValid(Sound))
	{
		/*VoiceComp = CreateVoiceAudioComponent(OutputSampleRate, NumOutChannels);
		VoiceComp->AddToRoot();*/
		SoundStreaming = CastChecked<USoundWaveProcedural>(Sound);
		if (SoundStreaming)
		{
			// Bind the GenerateData callback with FOnSoundWaveProceduralUnderflow object
			SoundStreaming->OnSoundWaveProceduralUnderflow = FOnSoundWaveProceduralUnderflow::CreateUObject(this, &UVoiceChatComponent::GenerateData);
		}
	}

	if (!SoundStreaming)
	{
		UKismetSystemLibrary::PrintString(this, FString("SoundStreaming is not valid"), true, true, FLinearColor::Red, 0.f);
		return;
	}
	//check(SoundStreaming);

	bool bIsPlaying = IsPlaying();
	if (bIsPlaying != bLastWasPlaying)
	{
		UE_LOG(LogVoice, Log, TEXT("VOIP audio component %s playing!"), bIsPlaying ? TEXT("is") : TEXT("is not"));
		bLastWasPlaying = bIsPlaying;
	}

	StarvedDataCount = (!bIsPlaying || (SoundStreaming->GetAvailableAudioByteCount() != 0)) ? 0 : (StarvedDataCount + 1);
	if (StarvedDataCount > 1)
	{
		UE_LOG(LogVoice, Log, TEXT("VOIP audio component starved %d frames!"), StarvedDataCount);
	}

	if (VoiceCapture.IsValid())
	{
		UE_LOG(LogVoice, Log, TEXT("Ciizas"));

		bool bDoWork = false;
		uint32 TotalVoiceBytes = 0;


		uint32 NewVoiceDataBytes = 0;
		EVoiceCaptureState::Type MicState = VoiceCapture->GetCaptureState(NewVoiceDataBytes);
		if (MicState == EVoiceCaptureState::Ok && NewVoiceDataBytes > 0)
		{
			//UE_LOG(LogVoice, Log, TEXT("Getting data! %d"), NewVoiceDataBytes);
			if (LastRemainderSize > 0)
			{
				// Add back any data from the previous frame
				VOICE_BUFFER_CHECK(RawCaptureData, LastRemainderSize);
				FMemory::Memcpy(RawCaptureData.GetData(), Remainder.GetData(), LastRemainderSize);
			}

			// Add new data at the beginning of the last frame
			uint64 SampleCount;
			MicState = VoiceCapture->GetVoiceData(RawCaptureData.GetData() + LastRemainderSize, NewVoiceDataBytes, NewVoiceDataBytes, SampleCount);
			TotalVoiceBytes = NewVoiceDataBytes + LastRemainderSize;

			UKismetSystemLibrary::PrintString(this, FString("New voice data bytes: ").Append(FString::FromInt(NewVoiceDataBytes)), true, true, FLinearColor::Red, 0.f);

			// Check to make sure this buffer has a valid, chronological buffer count.
			if (SampleCount <= CachedSampleCount)
			{
				UE_LOG(LogVoice, Log, TEXT("Out of order or ambiguous sample count detected! This sample count: %lu Previous sample count: %lu"), SampleCount, CachedSampleCount);
			}

			CachedSampleCount = SampleCount;

			//VOICE_BUFFER_CHECK(RawCaptureData, TotalVoiceBytes);
			UKismetSystemLibrary::PrintString(this, FString("RawCaptureData, TotalVoiceBytes: MicState: ").Append(FString::FromInt(TotalVoiceBytes)).Append(" ").Append(FString::FromInt(RawCaptureData.Num())).Append(" ").Append(EVoiceCaptureState::ToString(MicState)), true, true, FLinearColor::Red, 0.f);
			bDoWork = (MicState == EVoiceCaptureState::Ok);
		}


		if (bDoWork && TotalVoiceBytes > 0)
		{
			// At this point, we know that we have some valid data in our hands that is ready to play
			UKismetSystemLibrary::PrintString(this, FString("TotalVoiceBytes: ").Append(FString::FromInt(TotalVoiceBytes)), true, true, FLinearColor::Red, 0.f);

			// COMPRESSION BEGIN
			uint32 CompressedDataSize = 0;
			if (VoiceEncoder.IsValid())
			{
				CompressedDataSize = MaxCompressedDataSize;
				LastRemainderSize = VoiceEncoder->Encode(RawCaptureData.GetData(), TotalVoiceBytes, CompressedData.GetData(), CompressedDataSize);
				VOICE_BUFFER_CHECK(RawCaptureData, CompressedDataSize);

				if (LastRemainderSize > 0)
				{
					if (LastRemainderSize > MaxRemainderSize)
					{
						UE_LOG(LogVoice, Verbose, TEXT("Overflow!"));
						Remainder.AddUninitialized(LastRemainderSize - MaxRemainderSize);
						MaxRemainderSize = Remainder.Num();
					}

					VOICE_BUFFER_CHECK(Remainder, LastRemainderSize);
					FMemory::Memcpy(Remainder.GetData(), RawCaptureData.GetData() + (TotalVoiceBytes - LastRemainderSize), LastRemainderSize);
				}
			}
			// COMPRESSION END

			UKismetSystemLibrary::PrintString(this, FString("Data compressed: ArraySize: ").Append(FString::FromInt(CompressedData.Num())).Append("CompressedDataSize").Append(FString::FromInt(CompressedDataSize)), true, true, FLinearColor::Red, 0.f);

			// After the compressed data is placed on the buffer, place it on a clean array to transmit the size with the array and reduce the network weight (Lots of data is irrelevant)
			TArray<uint8> CompressedCulledData;
			CompressedCulledData.AddUninitialized(CompressedDataSize);
			FMemory::Memcpy(CompressedCulledData.GetData(), CompressedData.GetData(), CompressedDataSize);

			UKismetSystemLibrary::PrintString(this, FString("Data compressed: ArraySize: ").Append(FString::FromInt(CompressedCulledData.Num())).Append("CompressedDataSize").Append(FString::FromInt(CompressedDataSize)), true, true, FLinearColor::Red, 0.f);

			OnAudioCaptureCompleted.Broadcast(CompressedCulledData, true);

			//return;

			// DECOMPRESSION BEGIN
			uint32 UncompressedDataSize = 0;
			if (VoiceDecoder.IsValid() && CompressedDataSize > 0)
			{
				UKismetSystemLibrary::PrintString(this, FString("Decompressing Data"), true, true, FLinearColor::Red, 0.f);

				UncompressedDataSize = MaxUncompressedDataSize;
				VoiceDecoder->Decode(CompressedData.GetData(), CompressedDataSize,
					UncompressedData.GetData(), UncompressedDataSize);
				VOICE_BUFFER_CHECK(UncompressedData, UncompressedDataSize);
				UKismetSystemLibrary::PrintString(this, FString("Data uncompressed: CompressedArraySize: ").Append(FString::FromInt(CompressedDataSize)).Append("UncompressedDataSize").Append(FString::FromInt(UncompressedDataSize)), true, true, FLinearColor::Red, 0.f);
			}
			// DECOMPRESSION END

			const uint8* VoiceDataPtr = nullptr;
			uint32 VoiceDataSize = 0;

			if (bUseDecompressed)
			{
				if (UncompressedDataSize > 0)
				{
					//UE_LOG(LogVoice, Log, TEXT("Queuing uncompressed data! %d"), UncompressedDataSize);
					if (bZeroOutput)
					{
						FMemory::Memzero((uint8*)UncompressedData.GetData(), UncompressedDataSize);
					}

					VoiceDataSize = UncompressedDataSize;
					VoiceDataPtr = UncompressedData.GetData();
				}
			}
			else
			{
				//UE_LOG(LogVoice, Log, TEXT("Queuing original data! %d"), UncompressedDataSize);
				VoiceDataPtr = RawCaptureData.GetData();
				VoiceDataSize = (TotalVoiceBytes - LastRemainderSize);
			}

			if (VoiceDataPtr && VoiceDataSize > 0)
			{
				FScopeLock ScopeLock(&QueueLock);

				const int32 OldSize = UncompressedDataQueue.Num();
				const int32 AmountToBuffer = (OldSize + (int32)VoiceDataSize);
				if (AmountToBuffer <= MaxUncompressedDataQueueSize)
				{
					UncompressedDataQueue.AddUninitialized(VoiceDataSize);

					VOICE_BUFFER_CHECK(UncompressedDataQueue, AmountToBuffer);
					FMemory::Memcpy(UncompressedDataQueue.GetData() + OldSize, VoiceDataPtr, VoiceDataSize);
					CurrentUncompressedDataQueueSize += VoiceDataSize;
				}
				else
				{
					UE_LOG(LogVoice, Warning, TEXT("UncompressedDataQueue Overflow!"));
				}
			}

			// Wait for approx 1 sec worth of data before playing
			if (!bIsPlaying && (CurrentUncompressedDataQueueSize > (MaxUncompressedDataSize / 2)))
			{
				UE_LOG(LogVoice, Log, TEXT("Playback started"));
				UKismetSystemLibrary::PrintString(this, FString("Playback started"), true, true, FLinearColor::Red, 0.f);
				Play();
			}
		}
	}
}

void UVoiceChatComponent::PlayVoiceChatAudio(TArray<uint8> VoiceData, bool IsCompressed)
{
	UKismetSystemLibrary::PrintString(this, FString("Data received: ArraySize: ").Append(FString::FromInt(VoiceData.Num())), true, true, FLinearColor::Red, 0.f);

	TArray<uint8> AudioToPlay;

	uint32 UncompressedDataSize = 0;

	const uint8* VoiceDataPtr = nullptr;
	uint32 VoiceDataSize = 0;
	if (true)
	{
		// DECOMPRESSION BEGIN
		if (VoiceDecoder.IsValid() && VoiceData.Num() > 0)
		{
			AudioToPlay.AddUninitialized(MaxUncompressedDataSize);
			UncompressedDataSize = MaxUncompressedDataSize;
			VoiceDecoder->Decode(VoiceData.GetData(), VoiceData.Num(),
				AudioToPlay.GetData(), UncompressedDataSize);
			VOICE_BUFFER_CHECK(AudioToPlay, UncompressedDataSize);

			VoiceDataSize = UncompressedDataSize;
			VoiceDataPtr = AudioToPlay.GetData();

			UKismetSystemLibrary::PrintString(this, FString("Decompressed data: ArraySize: ").Append(FString::FromInt(UncompressedDataSize)), true, true, FLinearColor::Red, 0.f);
		}
		// DECOMPRESSION END
	}
	else
	{
		AudioToPlay = VoiceData;
		VoiceDataPtr = AudioToPlay.GetData();
		VoiceDataSize = AudioToPlay.Num();
	}

	if (VoiceDataPtr && VoiceDataSize > 0)
	{
		FScopeLock ScopeLock(&QueueLock);

		const int32 OldSize = UncompressedDataQueue.Num();
		const int32 AmountToBuffer = (OldSize + (int32)VoiceDataSize);
		if (AmountToBuffer <= MaxUncompressedDataQueueSize)
		{
			UncompressedDataQueue.AddUninitialized(VoiceDataSize);

			VOICE_BUFFER_CHECK(UncompressedDataQueue, AmountToBuffer);
			FMemory::Memcpy(UncompressedDataQueue.GetData() + OldSize, VoiceDataPtr, VoiceDataSize);
			CurrentUncompressedDataQueueSize += VoiceDataSize;
		}
		else
		{
			UE_LOG(LogVoice, Warning, TEXT("UncompressedDataQueue Overflow!"));
		}
	}

	// Wait for approx 1 sec worth of data before playing
	// TODO: This should be much lower: MaxUncompressedDataSize / 2 equals 1 sec
	// TODO: What happens if an amount that is smaller is sent?
	// TODO: If it is already playing, we will forget about this current bunch. We should store it
	if (!IsPlaying() && (CurrentUncompressedDataQueueSize > (MaxUncompressedDataSize / 4)))
	{
		UE_LOG(LogVoice, Log, TEXT("Playback started"));
		Play();
	}
}

void UVoiceChatComponent::InitAsListener()
{
	EncodeHint = EAudioEncodeHint::VoiceEncode_Audio;
	InputSampleRate = 48000;
	OutputSampleRate = 48000;
	NumInChannels = 2;
	NumOutChannels = 2;

	InitVoiceDecoder();

	USoundWaveProcedural* newSoundStreaming = NewObject<USoundWaveProcedural>();
	newSoundStreaming->SetSampleRate(OutputSampleRate);
	newSoundStreaming->NumChannels = NumOutChannels;
	newSoundStreaming->Duration = INDEFINITELY_LOOPING_DURATION;
	newSoundStreaming->SoundGroup = SOUNDGROUP_Voice;
	newSoundStreaming->bLooping = false;

	// Turn off async generation in old audio engine on mac.
#if PLATFORM_MAC
	if (!AudioDevice->IsAudioMixerEnabled())
	{
		newSoundStreaming->bCanProcessAsync = false;
	}
	else
#endif // #if PLATFORM_MAC
	{
		newSoundStreaming->bCanProcessAsync = true;
	}

	Sound = newSoundStreaming;
	bIsUISound = false;
	bAllowSpatialization = true;
	SetVolumeMultiplier(1.5f);

	const FSoftObjectPath VoiPSoundClassName = GetDefault<UAudioSettings>()->VoiPSoundClass;
	if (VoiPSoundClassName.IsValid())
	{
		SoundClassOverride = LoadObject<USoundClass>(nullptr, *VoiPSoundClassName.ToString());
	}
}

//bool UVoiceChatComponent::Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar)
//{
//	bool bWasHandled = false;
//	if (FParse::Command(&Cmd, TEXT("killtestvoice")))
//	{
//		delete this;
//		bWasHandled = true;
//	}
//	else if (FParse::Command(&Cmd, TEXT("vcstart")))
//	{
//		if (VoiceCapture.IsValid() && !VoiceCapture->IsCapturing())
//		{
//			VoiceCapture->Start();
//		}
//
//		bWasHandled = true;
//	}
//	else if (FParse::Command(&Cmd, TEXT("vcstop")))
//	{
//		if (VoiceCapture.IsValid() && VoiceCapture->IsCapturing())
//		{
//			VoiceCapture->Stop();
//		}
//
//		bWasHandled = true;
//	}
//	else if (FParse::Command(&Cmd, TEXT("vchint")))
//	{
//		// vchint <0/1>
//		FString NewHintStr = FParse::Token(Cmd, false);
//		if (!NewHintStr.IsEmpty())
//		{
//			EAudioEncodeHint NewHint = (EAudioEncodeHint)FPlatformString::Atoi(*NewHintStr);
//			if (NewHint >= EAudioEncodeHint::VoiceEncode_Voice && NewHint <= EAudioEncodeHint::VoiceEncode_Audio)
//			{
//				if (EncodeHint != NewHint)
//				{
//					EncodeHint = NewHint;
//
//					CleanupAudioComponent();
//					CleanupQueue();
//
//					VoiceEncoder = nullptr;
//					InitVoiceEncoder();
//
//					VoiceDecoder->Reset();
//				}
//			}
//		}
//
//		bWasHandled = true;
//	}
//	else if (FParse::Command(&Cmd, TEXT("vcdevice")))
//	{
//		// vcsample <device name>
//		FString NewDeviceName = FParse::Token(Cmd, false);
//		if (!NewDeviceName.IsEmpty())
//		{
//			bool bQuotesRemoved = false;
//			NewDeviceName = NewDeviceName.TrimQuotes(&bQuotesRemoved);
//			if (VoiceCapture.IsValid())
//			{
//				if (VoiceCapture->ChangeDevice(NewDeviceName, InputSampleRate, NumInChannels))
//				{
//					DeviceName = NewDeviceName;
//					CleanupAudioComponent();
//					CleanupQueue();
//					VoiceEncoder->Reset();
//					VoiceDecoder->Reset();
//					VoiceCapture->Start();
//				}
//				else
//				{
//					UE_LOG(LogVoice, Warning, TEXT("Failed to change device name %s"), *DeviceName);
//				}
//			}
//		}
//
//		bWasHandled = true;
//	}
//	else if (FParse::Command(&Cmd, TEXT("vcin")))
//	{
//		// vcin <rate> <channels>
//		FString SampleRateStr = FParse::Token(Cmd, false);
//		int32 NewInSampleRate = !SampleRateStr.IsEmpty() ? FPlatformString::Atoi(*SampleRateStr) : InputSampleRate;
//
//		FString NumChannelsStr = FParse::Token(Cmd, false);
//		int32 NewNumInChannels = !NumChannelsStr.IsEmpty() ? FPlatformString::Atoi(*NumChannelsStr) : NumInChannels;
//
//		if (NewInSampleRate != InputSampleRate ||
//			NewNumInChannels != NumInChannels)
//		{
//			InputSampleRate = NewInSampleRate;
//			NumInChannels = NewNumInChannels;
//
//			if (VoiceCapture.IsValid())
//			{
//				VoiceCapture->Shutdown();
//				VoiceCapture = nullptr;
//			}
//
//			InitVoiceCapture();
//
//			VoiceEncoder = nullptr;
//			InitVoiceEncoder();
//		}
//
//		bWasHandled = true;
//	}
//	else if (FParse::Command(&Cmd, TEXT("vcout")))
//	{
//		// vcout <rate> <channels>
//		FString SampleRateStr = FParse::Token(Cmd, false);
//		int32 NewOutSampleRate = !SampleRateStr.IsEmpty() ? FPlatformString::Atoi(*SampleRateStr) : OutputSampleRate;
//
//		FString NumChannelsStr = FParse::Token(Cmd, false);
//		int32 NewNumOutChannels = !NumChannelsStr.IsEmpty() ? FPlatformString::Atoi(*NumChannelsStr) : NumOutChannels;
//
//		if (NewOutSampleRate != OutputSampleRate ||
//			NewNumOutChannels != NumOutChannels)
//		{
//			OutputSampleRate = NewOutSampleRate;
//			NumOutChannels = NewNumOutChannels;
//
//			VoiceDecoder = nullptr;
//			InitVoiceDecoder();
//
//			CleanupAudioComponent();
//		}
//
//		bWasHandled = true;
//	}
//	else if (FParse::Command(&Cmd, TEXT("vcvbr")))
//	{
//		// vcvbr <true/false>
//		FString VBRStr = FParse::Token(Cmd, false);
//		int32 ShouldVBR = FPlatformString::Atoi(*VBRStr);
//		bool bVBR = ShouldVBR != 0;
//		if (VoiceEncoder.IsValid())
//		{
//			if (!VoiceEncoder->SetVBR(bVBR))
//			{
//				UE_LOG(LogVoice, Warning, TEXT("Failed to set VBR %d"), bVBR);
//			}
//		}
//
//		bWasHandled = true;
//	}
//	else if (FParse::Command(&Cmd, TEXT("vcbitrate")))
//	{
//		// vcbitrate <bitrate>
//		FString BitrateStr = FParse::Token(Cmd, false);
//		int32 NewBitrate = !BitrateStr.IsEmpty() ? FPlatformString::Atoi(*BitrateStr) : 0;
//		if (VoiceEncoder.IsValid() && NewBitrate > 0)
//		{
//			if (!VoiceEncoder->SetBitrate(NewBitrate))
//			{
//				UE_LOG(LogVoice, Warning, TEXT("Failed to set bitrate %d"), NewBitrate);
//			}
//		}
//
//		bWasHandled = true;
//	}
//	else if (FParse::Command(&Cmd, TEXT("vccomplexity")))
//	{
//		// vccomplexity <complexity>
//		FString ComplexityStr = FParse::Token(Cmd, false);
//		int32 NewComplexity = !ComplexityStr.IsEmpty() ? FPlatformString::Atoi(*ComplexityStr) : -1;
//		if (VoiceEncoder.IsValid() && NewComplexity >= 0)
//		{
//			if (!VoiceEncoder->SetComplexity(NewComplexity))
//			{
//				UE_LOG(LogVoice, Warning, TEXT("Failed to set complexity %d"), NewComplexity);
//			}
//		}
//
//		bWasHandled = true;
//	}
//	else if (FParse::Command(&Cmd, TEXT("vcdecompress")))
//	{
//		// vcdecompress <0/1>
//		FString DecompressStr = FParse::Token(Cmd, false);
//		int32 ShouldDecompress = FPlatformString::Atoi(*DecompressStr);
//		bUseDecompressed = (ShouldDecompress != 0);
//
//		bWasHandled = true;
//	}
//	else if (FParse::Command(&Cmd, TEXT("vcdump")))
//	{
//		if (VoiceCapture.IsValid())
//		{
//			VoiceCapture->DumpState();
//		}
//
//		if (VoiceEncoder.IsValid())
//		{
//			VoiceEncoder->DumpState();
//		}
//
//		if (VoiceDecoder.IsValid())
//		{
//			VoiceDecoder->DumpState();
//		}
//
//		bWasHandled = true;
//	}
//
//	return bWasHandled;
//}