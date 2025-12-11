// SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//

#pragma once

#include "CoreMinimal.h"
#include "Templates/PimplPtr.h"

#include "IGIModule.h"

class IGI_API FIGIGPT
{
public:
    FIGIGPT(FIGIModule* IGIModule);
    virtual ~FIGIGPT();

    void WarmUpPython(double TimeoutSec = 60.0);

    FString Evaluate(const FString& UserPrompt);

    void StartPersistentPython(double TimeoutSec = 30.0);
    void StopPersistentPython();

    FString EvaluateStructuredWithGrammar(const FString& UserPrompt, const FString& GrammarPath);

private:
    class Impl;
    TPimplPtr<class Impl> Pimpl;
};
