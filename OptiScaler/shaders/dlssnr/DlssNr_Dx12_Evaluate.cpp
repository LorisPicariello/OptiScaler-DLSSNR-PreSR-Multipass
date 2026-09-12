#include "pch.h"
#include "DlssNr_Dx12_State.h"

auto DlssNr_Dx12::State::EvaluateInternal(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params, bool beforeUpscale,
                          ID3D12CommandQueue* timingQueue, bool rayReconstruction, unsigned long long submissionEpoch,
                          bool interop) -> void
{
    std::lock_guard<std::recursive_mutex> nrLock(mutex);
    const Config& cfg = *Config::Instance();

    const unsigned finishedMode =
        !cfg.DlssNrFinishedPicture.value_or_default()                                             ? 0u
        : (cfg.DlssNrRunBeforeSr.value_or_default() || cfg.DlssNrDeferredDlss.value_or_default()) ? 2u
                                                                                                  : 1u;

    if (lastFinishedMode != finishedMode)
    {
        nr.reset = true;
        if (gpuTime)
            gpuTime->ClearLast();
        if (ngxTime)
            ngxTime->ClearLast();
        lastGpuTime.reset();
        lastNgxTime.reset();
        late.Cancel();
        deferredSr.Cancel();
        lastFinishedMode = finishedMode;
    }
    if (finishedMode)
    {
        nr.residualPair.Cancel();
        nr.residualStoreValid = false;
        nr.residualHistoryPrimed = false;
        if (!cfg.DlssNrEnabled.value_or_default())
        {
            deferredSr.Cancel();
            late.Cancel();
            return;
        }
        const bool dx11 = ::State::Instance().swapchainApi == API::DX11 ||
                          ::State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12;
        if ((interop || ::State::Instance().swapchainInteropApi != SwapchainInteropApi::None) && !dx11)
        {
            deferredSr.Cancel();
            late.Cancel();
            late.Say("Finished-picture NR supports DirectX 12 and the DirectX 11 bridge; Vulkan is not supported.");
            return;
        }
        if (finishedMode == 2)
        {
            if (rayReconstruction)
            {
                deferredSr.Cancel();
                late.Cancel();
                late.Say("Running the model before SR with this option does not support Ray Reconstruction.");
                return;
            }
            if (cmdList && params)
            {
                const auto submitted = ::State::Instance().frameCount;
                const auto epoch = seamClock.AtSeam(beforeUpscale, interop, submitted);
                if (beforeUpscale)
                    deferredSr.Before(cmdList, params, epoch, submitted, timingQueue, interop);
                else
                    deferredSr.After(cmdList, params, epoch);
            }
        }
        else
        {
            deferredSr.Cancel();
            if (beforeUpscale)
                late.Capture(cmdList, params, rayReconstruction);
        }
        return;
    }
    late.Cancel();
    if (beforeUpscale)
    {
        // Even a disabled/skipped pre pass must invalidate the previous result.
        if (nr.residualStoreValid)
            nr.residualHistoryPrimed = false;
        nr.residualPair.Cancel();
        nr.residualStoreValid = false;
    }
    const bool residualMode = cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrRunBeforeSr.value_or_default() &&
                              cfg.DlssNrResidualAcrossRr.value_or_default() && rayReconstruction;
    if (residualMode != nr.residualModeActive)
    {
        nr.residualPair.Cancel();
        nr.residualHistoryPrimed = false;
        nr.residualStoreValid = false;
        nr.residualModeActive = residualMode;
    }
    if (!cfg.DlssNrEnabled.value_or_default() || !cfg.DlssNrDeferredDlss.value_or_default() || rayReconstruction)
    {
        deferredSr.Cancel();
        if (rayReconstruction && cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrDeferredDlss.value_or_default())
            deferredSr.Say("inactive: Ray Reconstruction; using ordinary before/after NR placement");
    }
    else
    {
        if (cmdList != nullptr && params != nullptr)
        {
            const auto submitted = interop ? submissionEpoch : ::State::Instance().frameCount;
            const auto epoch = seamClock.AtSeam(beforeUpscale, interop, submitted);
            if (beforeUpscale)
                deferredSr.Before(cmdList, params, epoch, submitted, timingQueue, interop);
            else
                deferredSr.After(cmdList, params, epoch);
        }
        return;
    }

    if (!cfg.DlssNrEnabled.value_or_default())
    {
        ReportSkipOnce("it is switched off");
        return;
    }

    if (cmdList == nullptr || params == nullptr)
    {
        ReportSkipOnce("no command list or no parameter block");
        return;
    }

    // Both SR and RR+SR use the same placement control. Unsupported colour subrects
    // retain the common post-upscale fallback; RR identity only separates history
    // and prevents using the SR-only deferred-residual experiment on an RR feature.
    bool preSrCompatible = true;
    if (cfg.DlssNrRunBeforeSr.value_or_default())
    {
        ID3D12Resource* preColor = GetResource(params, NVSDK_NGX_Parameter_Color, "DLSSD.Color");
        unsigned int renderWidth = 0, renderHeight = 0, colorBaseX = 0, colorBaseY = 0;
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &renderWidth);
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &renderHeight);
        params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &colorBaseX);
        params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &colorBaseY);

        if (preColor == nullptr)
        {
            preSrCompatible = false;
        }
        else
        {
            const D3D12_RESOURCE_DESC colorDesc = preColor->GetDesc();
            const unsigned int allocationWidth = (unsigned int) colorDesc.Width;
            const unsigned int allocationHeight = colorDesc.Height;
            const auto active =
                DlssNr::PreSrColorExtent(colorDesc, renderWidth, renderHeight, colorBaseX, colorBaseY);
            preSrCompatible = active.has_value();

            if (active && (active->width != allocationWidth || active->height != allocationHeight))
            {

                if (!reportedPadding)
                {
                    reportedPadding = true;
                    LOG_INFO("DLSS-NR before SR: staging active {}x{} from padded Color allocation {}x{}; "
                             "only the active rectangle is copied back. Model size follows active size and "
                             "WorkingScale.",
                             active->width, active->height, allocationWidth, allocationHeight);
                }
            }

            if (!preSrCompatible)
            {

                if (!warnedSubrect)
                {
                    warnedSubrect = true;
                    LOG_WARN("DLSS-NR before SR requires a valid origin-zero active rectangle inside a "
                             "single-sample 2D Color texture; got allocation {}x{}, active {}x{} at {},{}. "
                             "Falling back after SR.",
                             allocationWidth, allocationHeight, renderWidth, renderHeight, colorBaseX, colorBaseY);
                }
            }
        }
    }

    const bool configuredBefore = cfg.DlssNrRunBeforeSr.value_or_default() && preSrCompatible;

    // ResidualAcrossRR: with RunBeforeSR + the game's Ray Reconstruction both on, run
    // the model before SR but leave Color untouched, then add its captured residual back after RR+SR.
    // Unlike every other placement this needs BOTH seams of one evaluate -- the before-upscale seam
    // falls through to the normal pre-SR body (which captures the residual and skips the copy-back),
    // and the after-upscale seam is handled here. If it is requested but the pre-SR path is not
    // compatible, configuredBefore is false and the normal gate runs plain post-SR NR instead.
    const bool residualAcrossRr =
        cfg.DlssNrResidualAcrossRr.value_or_default() && configuredBefore && rayReconstruction;

    if (residualAcrossRr && beforeUpscale &&
        (cfg.DlssNrHoldFrame.value_or_default() || cfg.DlssNrDebugView.value_or_default() != 0 ||
         cfg.DlssNrCompare.value_or_default() != 0 || cfg.DlssNrShowSkinMask.value_or_default()))
    {
        nr.residualHistoryPrimed = false;
        ReportSkipOnce("disable Hold frame, Compare and Debug view for residual-across-RR");
        return;
    }

    if (residualAcrossRr && !beforeUpscale)
    {
        ApplyResidualAcrossRr(cmdList, params);

        if (!announcedResidualAcrossRr)
        {
            announcedResidualAcrossRr = true;
            LOG_INFO("DLSS-NR: residual-across-RR active -- MV-reprojected accumulator, blend {:.2f}",
                     std::clamp(cfg.DlssNrResidualAcrossRrBlend.value_or_default(), 0.01f, 1.0f));
        }
        return;
    }

    if (configuredBefore != beforeUpscale)
        return;

    // Which of the game's APIs this evaluate arrived through.
    //
    // Says out loud what was previously only reasoned about: an FSR or XeSS title reaches this pass
    // transitively, because those shims call OptiScaler's own NVSDK_NGX_D3D12_EvaluateFeature and
    // this pass hangs off that. Nothing needed adding to the shims -- a call there would run the
    // model twice -- but "nothing needed adding" is a claim, and this is the line that checks it.
    {

        const ApiUpscalerInput api = ::State::Instance().currentInputApiName;

        if (saidApi != api)
        {
            saidApi = api;
            LOG_INFO("DLSS-NR reached through the game's {} input", ApiUpscalerInputName(api));
        }
    }

    ID3D12Resource* output = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    ID3D12Resource* target = beforeUpscale ? GetResource(params, NVSDK_NGX_Parameter_Color, "DLSSD.Color") : output;
    ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

    // Without all three there is nothing to run on. This is not a failure -- some evaluates legitimately
    // carry none of it -- so it stays quiet and tries again next frame.
    if (target == nullptr || depth == nullptr || motion == nullptr)
    {
        ReportSkipOnce(target == nullptr  ? (beforeUpscale ? "the parameters carried no color texture"
                                                           : "the parameters carried no output texture")
                       : depth == nullptr ? "the parameters carried no depth"
                                          : "the parameters carried no motion vectors");
        return;
    }

    unsigned int createFlags = featureFlags;
    if (!createFlags)
        params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);

    DlssNrFrameInfo frame {};
    frame.DepthInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    frame.MotionVectorsLowResolution = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
    if (output != nullptr)
    {
        frame.OutputWidth = (unsigned int) output->GetDesc().Width;
        frame.OutputHeight = output->GetDesc().Height;
    }
    unsigned int outputWidth = 0, outputHeight = 0;
    params->Get(NVSDK_NGX_Parameter_OutWidth, &outputWidth);
    params->Get(NVSDK_NGX_Parameter_OutHeight, &outputHeight);
    if (outputWidth && outputHeight)
    {
        frame.OutputWidth = outputWidth;
        frame.OutputHeight = outputHeight;
    }
    frame.BeforeUpscale = beforeUpscale;
    frame.RayReconstruction = rayReconstruction;
    frame.ResidualAcrossRr = residualAcrossRr; // only reaches here on the before-upscale seam
    if (residualAcrossRr)
    {
        if (!output)
            return;
        const auto outDesc = output->GetDesc();
        if (outDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || outDesc.SampleDesc.Count != 1 ||
            outDesc.DepthOrArraySize != 1 || outDesc.MipLevels != 1)
            return;
        if (nr.residualOutputWidth != outDesc.Width || nr.residualOutputHeight != outDesc.Height ||
            nr.residualOutputFormat != outDesc.Format)
        {
            ParkNrResource(nr.residualComposed);
            nr.residualHistoryPrimed = false;
            nr.residualOutputWidth = (unsigned) outDesc.Width;
            nr.residualOutputHeight = outDesc.Height;
            nr.residualOutputFormat = outDesc.Format;
        }
    }
    frame.SubmissionEpoch = interop ? submissionEpoch : ::State::Instance().frameCount;

    // Color and Output may use different formats even though DLSS treats them as the same frame colour
    // space. Output is the stable authority across injection points; target is only a fallback for a
    // malformed parameter block.
    ID3D12Resource* colourAuthority = output != nullptr ? output : target;
    frame.ColourIsLinearHdr = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0 &&
                              colourAuthority != nullptr &&
                              FormatCanHoldLinearHdr(colourAuthority->GetDesc().Format);

    // The game telling the upscaler to forget everything it has accumulated: a cut, a teleport, a
    // load. Every upscaler in this tree reads it and this pass did not, so the model's history was
    // only ever reset by things that happened to us -- a resize, a rebuild, a recovery from failure
    // -- and never by anything that happened in the game. Across a cut the model was reprojecting
    // the previous scene onto the new one and being asked to reconcile them.
    //
    // Read the same way FFXFeature_Dx12 reads it, including leaving it alone when the parameter is
    // absent: a game that never sets it is not asking for a reset every frame.
    {
        unsigned int gameReset = 0;

        if (params->Get(NVSDK_NGX_Parameter_Reset, &gameReset) == NVSDK_NGX_Result_Success)
            frame.Reset = gameReset != 0;
    }

    // How much of the guides is real. See DlssNrFrameInfo -- zero means the game did not say.
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &frame.RenderSubrectWidth);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &frame.RenderSubrectHeight);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &frame.DepthSubrectBaseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &frame.DepthSubrectBaseY);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &frame.MotionSubrectBaseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &frame.MotionSubrectBaseY);

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.MvScaleX) != NVSDK_NGX_Result_Success)
        frame.MvScaleX = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.MvScaleY) != NVSDK_NGX_Result_Success)
        frame.MvScaleY = 1.0f;

    // What the game says about its own exposure. Logged, used for nothing yet.
    //
    // The white point measured from the frame turned out to be a control loop rather than a
    // measurement: the pass writes into the buffer it reads, most games adapt their exposure to the
    // finished frame, and the two chase each other -- 0.01 to 97.9 in one Enshrouded session. Any
    // statistic taken from a frame we modify has that problem.
    //
    // These do not. DLSS.Pre.Exposure is the scale the game applied before handing the buffer over,
    // and ExposureTexture is a 1x1 the game fills with the exposure it is using; both are the game's
    // own numbers, decided upstream of anything here. Whether either is close to the divisor the model
    // actually wants is unknown, which is why this only prints them.
    //
    // The auto-exposure flag decides whether the texture means anything: with it set the game is
    // telling DLSS to work exposure out for itself and may supply nothing. OptiScaler forces that flag
    // on for eighteen games, so it is logged too -- reading a value whose flag has been overridden is
    // how the debug views lied earlier tonight.
    {
        float preExposure = 0.0f;
        const bool havePre =
            params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &preExposure) == NVSDK_NGX_Result_Success;

        void* exposureTex = nullptr;
        params->Get(NVSDK_NGX_Parameter_ExposureTexture, &exposureTex);

        frame.ExposureTexture = exposureTex;
        frame.PreExposure = havePre && preExposure > 1e-6f ? preExposure : 1.0f;

        nr.exposureOfferedNow = exposureTex != nullptr;
        nr.exposureEverOffered = nr.exposureEverOffered || nr.exposureOfferedNow;
        nr.exposureFrames++;

        const bool autoExposureFlag = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) != 0;

        const ExposureReport now { true, havePre ? preExposure : 0.0f, havePre, exposureTex != nullptr,
                                   autoExposureFlag };

        if (!logged.valid || logged.havePre != now.havePre || logged.haveTexture != now.haveTexture ||
            logged.autoFlag != now.autoFlag ||
            std::abs(logged.pre - now.pre) > std::max(0.01f * std::abs(now.pre), 1e-4f))
        {
            logged = now;
            LOG_INFO("DLSS-NR exposure from the game: DLSS.Pre.Exposure {}, ExposureTexture {}, "
                     "auto-exposure flag {}",
                     now.havePre ? std::to_string(now.pre) : std::string("not supplied"),
                     now.haveTexture ? "supplied" : "not supplied", now.autoFlag ? "set" : "clear");
        }

        // The value itself, once it has come back off the GPU. Separate from the line above because
        // that one says what the game offers and this one says what it actually reads -- and because
        // the reading arrives three frames after the offer.

        if (nr.gameExposure > 1e-6f &&
            std::abs(loggedExposure - nr.gameExposure) > std::max(0.02f * nr.gameExposure, 1e-5f))
        {
            loggedExposure = nr.gameExposure;
            LOG_INFO("DLSS-NR game exposure {:.5f} (pre-exposure {:.3f}) -> white point would be {:.2f}",
                     nr.gameExposure, nr.gamePreExposure, nr.gamePreExposure / nr.gameExposure);
        }

        // The scan's number, on the same cadence, so one log carries both.
        //
        // This is the whole validation. In a game that hands over an exposure texture there is a
        // known-correct value; if the scan's candidate tracks it, the scan found the right buffer
        // rather than merely a moving one, and can be trusted where a game hands over nothing.
        // Comparing two numbers after the fact needs both written down, and until now the scan's
        // value existed only in a menu nobody can read while playing.
        {
            int which = 0;
            float low = 0.0f, high = 0.0f;
            const float scanned = DlssNr::ExposureScan::BestValue(&which, &low, &high);

            if (scanned > 0.0f && std::abs(loggedScan - scanned) > std::max(0.02f * scanned, 1e-6f))
            {
                loggedScan = scanned;

                if (nr.gameExposure > 1e-6f)
                    LOG_INFO("DLSS-NR exposure scan: candidate {} = {:.5f} ({:.5f}..{:.5f})  |  the "
                             "game's own exposure is {:.5f}  |  ratio {:.4f}",
                             which, scanned, low, high, nr.gameExposure, scanned / nr.gameExposure);
                else
                    LOG_INFO("DLSS-NR exposure scan: candidate {} = {:.5f} ({:.5f}..{:.5f})  |  this "
                             "game supplies no exposure to compare against",
                             which, scanned, low, high);
            }
        }
    }

    // The upscaler's inputs are at render resolution while colour and output are at display
    // resolution; the model takes that as a subrect per resource, which the pass reads from the
    // resources themselves.
    ID3D12Device* device = nullptr;

    if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        ReportSkipOnce("the output texture belongs to no D3D12 device");
        return;
    }

    // The pass is the object, so the caller holds it. Built once, on the device the frame is on.

    device->Release();

    Run(cmdList, target, depth, motion, target, frame, timingQueue);
    if (residualAcrossRr && nr.residualStoreValid)
        nr.residualPair.Arm(cmdList, params, output);
}

auto DlssNr_Dx12::State::ApplyResidualAcrossRr(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params) -> void
{
    auto* output = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    const bool paired = nr.residualPair.Take(cmdList, params, output);
    const bool ready = nr.residualStoreValid;
    nr.residualStoreValid = false;
    if (!paired || !ready || !output || !nr.residualComposed)
    {
        nr.residualHistoryPrimed = false;
        return;
    }
    const auto& cfg = *Config::Instance();
    const float strength = cfg.DlssNrTransferStrength.value_or_default();
    if (!cfg.DlssNrApplyModel.value_or_default() || !std::isfinite(strength) || strength <= 0.0f)
    {
        nr.residualHistoryPrimed = false;
        return;
    }
    if ((cfg.RestoreComputeSignature.value_or_default() || cfg.RestoreGraphicSignature.value_or_default()) &&
        !D3D12Hooks::CanRestoreRootSignature(cmdList))
        return;
    lifetime.Record(cmdList);
    ScopedNrStateEnvelope stateEnvelope(cmdList);
    const auto desc = output->GetDesc();
    const auto carrier = nr.residualComposed->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
        desc.Width != carrier.Width || desc.Height != carrier.Height || desc.Format != carrier.Format ||
        desc.DepthOrArraySize != 1 || desc.MipLevels != 1)
        return;
    auto arrival = cfg.OutputResourceBarrier.has_value() ? (D3D12_RESOURCE_STATES) cfg.OutputResourceBarrier.value()
                                                         : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Barrier(cmdList, output, arrival, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, nr.residualComposed, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    DlssNrConstants apply {};
    apply.Mode = DlssNrResidualMode_Apply;
    apply.Width = (unsigned) desc.Width;
    apply.Height = desc.Height;
    apply.TransferStrength = std::clamp(strength, 0.0f, 1.0f);
    const bool composed =
        shader.DispatchResidualPass(cmdList, apply, output, nr.residualHistory[nr.residualHistoryIndex & 1u],
                                    nullptr, nullptr, nr.residualComposed);
    if (composed)
    {
        Barrier(cmdList, nr.residualComposed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmdList, output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyResource(output, nr.residualComposed);
        Barrier(cmdList, output, D3D12_RESOURCE_STATE_COPY_DEST, arrival);
        Barrier(cmdList, nr.residualComposed, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    else
    {
        Barrier(cmdList, output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, arrival);
        Barrier(cmdList, nr.residualComposed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        nr.residualHistoryPrimed = false;
    }
}
