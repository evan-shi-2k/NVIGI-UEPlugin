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

    FString Evaluate(const FString& UserPrompt);

    FString EvaluateStructured(const FString& UserPrompt);

private:
    class Impl;
    TPimplPtr<class Impl> Pimpl;
};
