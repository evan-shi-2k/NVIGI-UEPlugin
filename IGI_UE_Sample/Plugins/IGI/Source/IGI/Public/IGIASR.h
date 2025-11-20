// SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES.
// SPDX-License-Identifier: MIT
//

#pragma once

#include "CoreMinimal.h"
#include "Templates/PimplPtr.h"

#include "IGIModule.h"

/**
 * Thin wrapper over an NVIGI ASR instance (e.g., Whisper).
 *
 * NOTE: This is a skeleton. All NVIGI calls are TODOs in the .cpp.
 */
class IGI_API FIGIASR
{
public:
    // Non-owning pointer to IGI module (same pattern as FIGIGPT).
    explicit FIGIASR(FIGIModule* InIGIModule);
    virtual ~FIGIASR();

    /**
     * Blocking, single-shot transcription of PCM16 audio.
     *
     * @param PCM16        Interleaved signed 16-bit PCM samples.
     * @param SampleRateHz Sample rate (e.g., 16000).
     * @param NumChannels  Number of channels (1 = mono, 2 = stereo).
     * @param bIsFinal     Whether this is a final utterance vs. a partial segment.
     * @return             UTF-8 text mapped to FString. Empty string on error.
     */
    FString TranscribePCM16(
        const TArray<int16>& PCM16,
        int32 SampleRateHz,
        int32 NumChannels,
        bool bIsFinal = true);

    /**
     * Optional helper overload for float PCM in [-1, 1].
     * Internally converts to 16-bit and forwards to TranscribePCM16.
     */
    FString TranscribePCMFloat(
        const TArray<float>& PCMFloat,
        int32 SampleRateHz,
        int32 NumChannels,
        bool bIsFinal = true);

private:
    class Impl;
    TPimplPtr<class Impl> Pimpl;
};
