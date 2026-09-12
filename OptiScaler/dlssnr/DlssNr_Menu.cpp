#include "pch.h"
#include "DlssNrFeature_Vk.h"

#include "DlssNr.h"
#include "DlssNr_ExposureScan.h"
#include "DlssNr_PipelineUi.h"

#include <Config.h>
#include <menu/menu_common.h>

#include <imgui/imgui.h>

#include <string>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace DlssNr
{

// The "(?)" marker every control carries, matching the rest of the menu.
static void HelpMarker(const char* tip)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");

    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// A slider that only writes its value when the handle is released.
//
// Some controls -- intensity, the structure and tone strengths -- are read by the model once, when
// the feature is built, so changing one rebuilds the whole feature. Writing on every pixel of a drag
// meant a rebuild per frame, felt as the picture hitching while you scrub. The slider still tracks
// live under the cursor; only the commit that triggers the rebuild waits for release. Cheap controls
// that are just shader constants (detail, colour, paper white) do not use this -- they can afford to
// apply live.
template <typename Option>
static bool DeferredSlider(const char* label, Option* opt, float mn, float mx, float def, const char* fmt = "%.2f",
                           bool inheritReset = false)
{
    static std::unordered_map<ImGuiID, float> pending;
    const ImGuiID id = ImGui::GetID(label);

    auto it = pending.find(id);
    float value = it != pending.end() ? it->second : (opt->has_value() ? opt->value() : def);
    bool changed = false;

    if (ImGui::SliderFloat(label, &value, mn, mx, fmt))
        pending[id] = value;

    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        auto committed = pending.find(id);

        if (committed != pending.end())
        {
            *opt = std::clamp(committed->second, mn, mx);
            pending.erase(committed);
            changed = true;
        }
    }

    ImGui::SameLine();

    const std::string resetId = std::string("Reset##") + label;
    if (ImGui::SmallButton(resetId.c_str()))
    {
        if (inheritReset)
            *opt = std::optional<float> {};
        else
            *opt = def;
        pending.erase(id);
        changed = true;
    }

    if (std::strcmp(label, "Intensity") == 0)
        HelpMarker("Overall enhancement strength for this pass. 1 = default; results depend on the profile.\nValues "
                   "above 1 are experimental; the runtime may clamp or ignore them.");
    else if (std::strcmp(label, "Local structure") == 0)
        HelpMarker("Fine detail and local contrast requested from the model (high-frequency structure).\n1 = default; "
                   "values above 1 are experimental.");
    else if (std::strcmp(label, "Local tone") == 0)
        HelpMarker("Broad brightness and lighting changes requested from the model (low-frequency tone).\nLater passes "
                   "default to 0. Values above 1 are experimental.");
    else if (std::strcmp(label, "Skin structure") == 0)
        HelpMarker("Fine detail for pixels the model identifies as skin. -1 follows Local structure; 0 reduces skin "
                   "detail.\nSkin colour is controlled separately. Values above 1 are experimental.");
    return changed;
}

// An absent later-pass setting inherits pass 1. The first combo item represents that absence; the
// remaining items map directly to the model's zero-based profile values.
static bool InheritedProfileCombo(const char* label, CustomOptional<uint32_t, NoDefault>* opt, const char* const* names,
                                  int nameCount)
{
    int selected = 0;

    if (opt->has_value())
        selected = std::clamp((int) opt->value(), 0, nameCount - 2) + 1;

    if (!ImGui::Combo(label, &selected, names, nameCount))
        return false;

    if (selected == 0)
        *opt = std::optional<uint32_t> {};
    else
        *opt = (uint32_t) (selected - 1);

    return true;
}

static void RenderPlacement(Config* config, float menuResScale)
{
    const bool enabled = config->DlssNrEnabled.value_or_default();
    bool finishedPicture = config->DlssNrFinishedPicture.value_or_default();
    if (ImGui::Checkbox("Apply NR to the finished picture", &finishedPicture))
    {
        config->DlssNrFinishedPicture = finishedPicture;
        DlssNr::RetryAfterFailure();
    }
    HelpMarker(
        "Apply NR after the game has finished its lighting and effects. This may help with green noise.\nWorks with "
        "frame generation on or off in native DirectX 12 games, including SDR, HDR10 and scRGB.\nIt can also change "
        "the HUD and menus. Enable Run the model before Super Resolution to generate the changes earlier.");
    if (finishedPicture && enabled)
    {
        const auto feature = State::Instance().currentFeature;
        if (feature && (feature->Api() != API::DX12 || feature->IsWithDx12()))
            ImGui::TextWrapped("This option needs a native DirectX 12 game.");
        else
            ImGui::TextWrapped("%s", DlssNr::FinishedPictureStatus().c_str());
    }

    bool beforeSr = config->DlssNrRunBeforeSr.value_or_default() ||
                    (finishedPicture && config->DlssNrDeferredDlss.value_or_default());
    const auto activeFeature = State::Instance().currentFeature;
    const bool rayReconstruction = activeFeature && activeFeature->GetUpscalerType() == Upscaler::DLSSD;
    const bool deferredActive = !finishedPicture && config->DlssNrDeferredDlss.value_or_default() && !rayReconstruction;
    if (deferredActive)
        ImGui::BeginDisabled();
    if (ImGui::Checkbox(finishedPicture ? "Run the model before Super Resolution" : "Apply before Super Resolution",
                        &beforeSr))
    {
        config->DlssNrRunBeforeSr = beforeSr;
        if (finishedPicture)
            config->DlssNrDeferredDlss = false;
    }
    if (deferredActive)
        ImGui::EndDisabled();

    HelpMarker(finishedPicture ? "Run the model at the smaller input size, upscale its changes with DLSS, then apply "
                                 "them to the finished picture.\nExperimental: the colour transfer is approximate and "
                                 "may look different. Requires DLSS SR; does not support RR."
                               : "On: apply NR before SR or combined RR+SR. Off: apply it afterward.\nBefore RR is "
                                 "experimental. Unsupported input layouts fall back after upscaling.");

    if (!finishedPicture)
    {
        bool residualAcrossRr = config->DlssNrResidualAcrossRr.value_or_default();
        ImGui::BeginDisabled(deferredActive || !beforeSr);
        if (ImGui::Checkbox("Carry the pre-SR edit across RR (experimental)", &residualAcrossRr))
            config->DlssNrResidualAcrossRr = residualAcrossRr;
        ImGui::EndDisabled();
        HelpMarker("Only with Apply before Super Resolution on and the game's Ray Reconstruction active.\nRuns the "
                   "model before SR but leaves the colour input untouched, then adds its edit back onto the RR+SR "
                   "output so it survives RR's denoise.\nThe edit is carried as a motion-vector-reprojected temporal "
                   "accumulator: the per-frame ray-trace noise averages out, the enhancement stays. Inert otherwise.");

        ImGui::BeginDisabled(deferredActive || !beforeSr || !residualAcrossRr);
        float residualBlend = config->DlssNrResidualAcrossRrBlend.value_or_default();
        if (ImGui::SliderFloat("Detail accumulation rate", &residualBlend, 0.01f, 1.0f, "%.2f"))
            config->DlssNrResidualAcrossRrBlend = std::clamp(residualBlend, 0.01f, 1.0f);
        ImGui::EndDisabled();
        HelpMarker("How fast the carried edit builds up. Lower = stabler but slower to appear; 1.0 = no accumulation "
                   "(each frame's raw residual, which flickers). Default 0.08.");
    }
    bool deferredDlss = config->DlssNrDeferredDlss.value_or_default();
    if (!finishedPicture)
    {
        if (ImGui::Checkbox("Generate before SR, apply after SR (DLSS)", &deferredDlss))
            config->DlssNrDeferredDlss = deferredDlss;
        HelpMarker("Compute NR at input resolution, upscale its changes with DLSS, then apply them after "
                   "SR.\nExperimental: may flicker and adds GPU cost. Requires DLSS on DX12 or its bridges; does not "
                   "support RR.\nOverrides Apply before Super Resolution. Disable Hold frame, Compare and Debug view.");
        if (deferredDlss && rayReconstruction)
            ImGui::TextWrapped("Generate before / apply after is unavailable with RR. Apply before Super Resolution "
                               "controls NR placement.");
        else if (deferredDlss)
            ImGui::TextWrapped("Residual DLSS: %s", DlssNr::DeferredDlssStatus().c_str());
    }
    else if (beforeSr)
        ImGui::TextWrapped("Pre-SR changes: %s", DlssNr::DeferredDlssStatus().c_str());

    // The toggle can be bound to a key, and nobody would think to look for it under Keybinds
    // unless told. Dimmed, because it is a note rather than a setting.
    ImGui::TextDisabled("Set the NR toggle shortcut under Keybinds.");
}

static void RenderStatus(Config* config, float menuResScale)
{
    const bool enabled = config->DlssNrEnabled.value_or_default();
    const bool finishedPicture = config->DlssNrFinishedPicture.value_or_default();
    // Either backend. The two keep separate state, and on a native Vulkan game the D3D12 side
    // is never touched -- so asking only that one reports "waiting for the upscaler" over a pass
    // that is demonstrably running.
    const bool vulkan = DlssNr::IsRunningVk();

    // Turning the pass off does not release the model, so the feature handle stays alive and
    // IsRunning keeps answering yes. Reporting a cost from that was wrong in the way that matters
    // most: the toggle is how anyone A/Bs this, so the one moment the number is read is the one
    // moment it describes the frame before last.
    if (!enabled)
    {
        ImGui::TextDisabled("NR off.");
    }
    else if (!DlssNr::IsRunning() && !vulkan)
    {
        const auto feature = State::Instance().currentFeature;
        const bool nativeVk = feature && feature->Api() == API::Vulkan && !feature->IsWithDx12();
        const char* reason = nativeVk ? DlssNr::FailureReasonVk() : DlssNr::FailureReason();

        if (reason[0] != 0)
        {
            ImGui::Text("Off for this session: %s.", reason);
            ImGui::SameLine();

            if (nativeVk)
                ImGui::TextUnformatted("Restart the game to retry native Vulkan NR.");
            else if (ImGui::SmallButton("Retry"))
                DlssNr::RetryAfterFailure();
        }
        else if (feature && feature->Api() == API::DX11 && !feature->IsWithDx12())
        {
            ImGui::TextWrapped("NR needs the D3D12 bridge on D3D11. Choose an upscaler marked w/Dx12 and restart.");
        }
        else if (nativeVk && config->DlssNrDeferredDlss.value_or_default())
        {
            ImGui::TextWrapped("Disable Generate before SR, apply after SR (DLSS) to use native Vulkan NR.");
        }
        else if (enabled)
            ImGui::TextUnformatted("Waiting for the upscaler to run.");
    }
    else
    {
        // The elapsed time belongs here rather than only in the upscaler's breakdown: that tooltip needs
        // OptiScaler's own upscaler to have run, and with native DLSS passing through there is
        // nothing in it to hang this off.
        // Either backend's timer. They measure the same thing by different means, and only one
        // of them is running.
        const auto ms = vulkan ? DlssNr::LastGpuTimeVk() : DlssNr::LastGpuTime();

        // With "Apply the model" off the pass STILL RUNS (so Hold-frame A/B can toggle its edit on
        // a frozen frame) -- it only outputs the clean frame.
        // Enable Neural Rendering off stops the work.
        const char* runSuffix = !config->DlssNrApplyModel.value_or_default() ? "  (model running, edit hidden)" : "";

        // Keep the running indicator green, using the theme's HDR-adjusted text brightness.
        const auto textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text,
                             ImVec4(textColor.x * 0.55f, textColor.y * 0.80f, textColor.z * 0.55f, textColor.w));
        if (ms.has_value())
            ImGui::Text("Running%s - %.2f ms elapsed%s", vulkan ? " natively on Vulkan" : "", ms.value(), runSuffix);
        else if (vulkan)
            // Measured but not yet read: the first few frames are still in the query ring.
            ImGui::Text("Running natively on Vulkan - %llu frames%s", DlssNr::FramesVk(), runSuffix);
        else
            ImGui::Text("Running.%s", runSuffix);
        ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Time between the start and end of NR on the GPU, including delays while other work "
                              "runs.\nCompare FPS to check the effect on game performance.");
        if (finishedPicture)
            ImGui::TextDisabled("Includes time shared with other GPU work.");
    }
}

static void RenderInput(Config* config, float menuResScale)
{
    // longer survives being enlarged onto it.
    // Applied when the handle is let go, not while it is moving.
    //
    // Every distinct value here is a different working size, and a different working size tears
    // down the scratch textures and rebuilds the model. Writing it on each pixel of a drag meant
    // dozens of rebuilds in a second, which is felt as the whole frame hitching. The slider still
    // reads live; only the commit waits.
    static int pendingScale = -1;

    int scalePercent =
        pendingScale >= 0 ? pendingScale : (int) lroundf(config->DlssNrWorkingScale.value_or_default() * 100.0f);

    if (ImGui::SliderInt("Model resolution", &scalePercent, 25, 200, "%d%%"))
        pendingScale = scalePercent;

    if (ImGui::IsItemDeactivatedAfterEdit() && pendingScale >= 0)
    {
        config->DlssNrWorkingScale = std::clamp(pendingScale, 25, 200) / 100.0f;
        pendingScale = -1;
    }

    HelpMarker("NR resolution relative to the image it processes. 50% halves width and height; 100% uses the full "
               "size.\nLower values reduce cost and fine detail. Above 100% increases cost. Game output resolution is "
               "unchanged.");

    if (scalePercent > 100)
        ImGui::TextDisabled("NR scale: %.2fx. Higher resolution increases GPU cost.", scalePercent / 100.0f);

    if (scalePercent > 100)
    {
        static const char* dsNames[] = { "FSR1",     "Bicubic", "Catmull-Rom", "Lanczos2",
                                         "Lanczos3", "Kaiser2", "Kaiser3",     "MAGIC" };
        int ds = (int) config->DlssNrScalingDownscaler.value_or_default();
        if (ds < 0 || ds >= IM_ARRAYSIZE(dsNames))
            ds = (int) Scaler::Lanczos3;

        if (ImGui::Combo("Downscaler (NR)", &ds, dsNames, IM_ARRAYSIZE(dsNames)))
            config->DlssNrScalingDownscaler = (Scaler) ds;

        HelpMarker("Filter used to reduce NR output when Model resolution exceeds 100%.\nSharper filters may introduce "
                   "ringing around edges.");
    }

    // Meaningful only when the model runs BELOW the frame's size. At 100% -- and above, where
    // supersampling composites its down-legged answer at native -- the residual collapses to the
    // model's own picture and the two modes are identical, so the control says so by going grey.
    {
        const bool reduced = config->DlssNrWorkingScale.value_or_default() < 0.999f;

        if (!reduced)
            ImGui::BeginDisabled();

        static const char* enlargeNames[] = { "Classic", "Matched residual" };
        int enlarge = config->DlssNrTransfer.value_or_default() == 1 ? 1 : 0;

        if (ImGui::Combo("Enlargement", &enlarge, enlargeNames, IM_ARRAYSIZE(enlargeNames)))
            config->DlssNrTransfer = (uint32_t) enlarge;

        if (!reduced)
            ImGui::EndDisabled();

        HelpMarker("Below 100% model resolution: Classic enlarges the model output; Matched residual enlarges only its "
                   "changes.\nMatched residual can reduce blur and colour shifts. No effect at 100% or above.");
    }

    // Experimental. 0 off (soft knee), 1 Neutwo + our composition, 2 Neutwo + pure-inverse replace,
    // 3 hybrid+composed, 4 hybrid+replace (identity midtones + unclipped highlights). Always shown.
    static const char* reversibleNames[] = { "Off (soft knee)", "Neutwo proxy + composed", "Neutwo proxy + replace",
                                             "Hybrid proxy + composed", "Hybrid proxy + replace" };
    int reversible = (int) config->DlssNrReversibleMode.value_or_default();
    if (reversible < 0 || reversible > 4)
        reversible = 0;
    if (ImGui::Combo("HDR mapping (experimental)", &reversible, reversibleNames, IM_ARRAYSIZE(reversibleNames)))
        config->DlssNrReversibleMode = (uint32_t) reversible;

    HelpMarker("Choose how HDR brightness is mapped for NR.\nSoft knee compresses highlights. Neutwo uses a reversible "
               "curve. Hybrid preserves midtones and compresses highlights.\nComposed uses the strength and highlight "
               "controls. Replace bypasses them and may flicker.");

    ImGui::TextDisabled("HDR input settings. Adjust the brightness range presented to NR.");

    {
        // Logarithmic, because the useful range is not linear. A quarter to 240: the low end because
        // a frame the game already tone mapped wants roughly 1, the high end because there is no
        // principled ceiling -- this is a divisor on an open-ended linear buffer, and how far up a
        // given game needs to go is a property of that game's exposure, not of anything we can bound.
        // One tester was still improving at 100. A linear slider over that span would spend nine
        // tenths of its travel on values nobody needs and never reach the ones they do.
        // One dropdown, because there is one answer.
        //
        // This was two checkboxes that could both be on, and every attempt to stop that was a patch
        // on a shape that should not have existed. Greying deadlocked -- each disabled the other, so
        // once both were set the only way out was a button the notice never mentioned. Clearing
        // worked but silently undid a setting somebody had made. Both were ways to stop an illegal
        // state being REACHED; a single choice cannot reach it, because there is only one value to
        // be in.
        //
        // Each option also says whether it can actually do anything in THIS game, so the
        // choice is made on what is available rather than on what sounds best.
        {
            const auto ex = DlssNr::GameExposureStatus();
            const bool vk = DlssNr::IsRunningVk();
            const bool haveExposure = vk ? DlssNr::ExposureOfferedVk() : ex.everOffered;

            const float anchorNow = DlssNr::ExposureScan::BestValue();
            const bool haveAnchor = !DlssNr::ExposureScan::Anchors().empty();

            static const char* sourceNames[] = { "Manual paper white", "Game exposure",
                                                 "Scanned exposure (experimental)" };

            int source = (int) config->DlssNrWhitePointSource.value_or_default();

            if (source < 0 || source > 2)
                source = 0;

            if (ImGui::Combo("White point source", &source, sourceNames, IM_ARRAYSIZE(sourceNames)))
            {
                config->DlssNrWhitePointSource = (uint32_t) source;

                // Nothing else to set. The scan asks the source whether it is wanted, so choosing
                // it here is the whole of switching it on -- there is no second flag to keep in
                // step, and so no way for the two to disagree.
            }

            HelpMarker("Manual: use Paper white. Game exposure: use exposure supplied by the game.\nScanned exposure: "
                       "estimate it from game buffers; requires calibration and may select the wrong buffer.");

            // Availability for the option currently chosen, using the HDR-adjusted theme.
            if (source == 1)
            {
                if (!vk && ex.seenFrames == 0)
                    ImGui::TextDisabled("Waiting for a frame...");
                else if (!haveExposure)
                    ImGui::TextDisabled("No game exposure available. Using manual paper white.");
                else if (vk)
                    ImGui::TextDisabled("Using game exposure.");
                else if (ex.exposure > 1e-6f)
                {
                    const float trim = std::clamp(config->DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);
                    ImGui::TextDisabled("Game exposure %.4f  ->  white point %.2f%s", ex.exposure,
                                        ex.preExposure / ex.exposure * trim,
                                        ex.offeredNow ? "" : "  (held: absent this frame)");
                }
                else
                    ImGui::TextDisabled("Reading exposure...");
            }
            else if (source == 2)
            {
                // "Nothing found" and "found several, none of them moving" are different states,
                // and this said the first for both. In GTA V the log carried eight candidates while
                // the panel claimed there were none, which reads as the scan being broken when what
                // it actually needs is for the light to change.
                if (anchorNow <= 0.0f)
                {
                    const unsigned int watching = (unsigned int) DlssNr::ExposureScan::Report().size();

                    if (watching == 0)
                        ImGui::TextDisabled("No exposure candidates found.");
                    else
                        ImGui::TextDisabled("%u candidates; move between bright and dark areas to test them.",
                                            watching);
                }
                else if (!haveAnchor)
                    ImGui::TextDisabled("Exposure candidate found. Adjust Paper white, then select Anchor here.");
                // Once anchored, the scan -> white point readout sits above the sliders below; it is
                // not repeated up here.
            }
            else if (haveExposure)
            {
                ImGui::TextDisabled("Game exposure is available.");
            }
        }

        // A measured suggestion for paper white used to sit here and has been withdrawn.
        //
        // It took the 90th percentile of per-tile peak luminance from the untouched frame, which is a
        // statement about scene content rather than about the buffer's scale. In Nioh 3, where the
        // right answer is about 240, it offered 8 -- because most tiles are shadow and the percentile
        // sits wherever most tiles are. The guard meant to catch that compared each tile against the
        // frame's own brightest, which is scale-free and therefore passes on a black screen: the same
        // relative-threshold mistake the white point meter was removed for, made a second time.
        //
        // A wrong number offered confidently is worse than no number, so nothing is offered. What
        // replaces it has to be a measurement of the game's own exposure rather than of its scenery:
        // the exposure texture where a game supplies one, and otherwise the ratio between the
        // scene-referred buffer and the finished frame, which is that exposure by definition.

        // Two controls, not one control with two meanings.
        //
        // These are different quantities. The manual path wants an absolute divisor on an open-ended
        // linear buffer -- Nioh 3 needs about 240 -- and the exposure path wants a multiplier on a
        // number the game already supplied, where 1 is correct and anything far from it says the read
        // is wrong rather than that somebody prefers it.
        //
        // They used to share one stored value, narrowed to 0.25..4 when the toggle was on. That kept
        // a ruinous value unreachable but left two worse problems: moving the slider in one mode
        // silently destroyed the number found in the other, and there was no way back to "just take
        // the game's answer" short of knowing that the number for it was 1. Separate values fix both.
        // Switching modes is now non-destructive in both directions.
        // The trim belongs to both automatic sources, since both end in "the game's number times a
        // little". Only the manual source gets the absolute slider.
        // One slider per source, each remembering its own number.
        //
        // A trim on the game's exposure and a trim on a buffer the scan found are trims on different
        // things, and a value found against one means nothing against the other. Sharing them meant
        // changing source silently carried a number across, so a picture that had been tuned came
        // back wrong for a reason nothing on screen explained.
        //
        // The scan before it is anchored is the exception, and it has to be: anchoring captures an
        // absolute white point, so there must be an absolute slider to set. Showing a trim there
        // asked people to "set paper white below" next to a control that was not paper white.
        const int wpSource = (int) config->DlssNrWhitePointSource.value_or_default();

        // Which anchor row the paper-white slider edits, or -1 for the live unanchored point. Menu-
        // local and not persisted; the anchor block below sets it when a row is clicked. Declared
        // here because both the slider (this block) and the table (below) read it in the same frame.
        static int selectedAnchor = -1;
        auto anchors = DlssNr::ExposureScan::Anchors();
        if (selectedAnchor >= (int) anchors.size())
            selectedAnchor = -1;

        if (wpSource == 2)
        {
            const bool editingRow = selectedAnchor >= 0 && selectedAnchor < (int) anchors.size();

            // The single scan -> white point readout, above the sliders it explains.
            if (!anchors.empty())
            {
                const float liveScan = DlssNr::ExposureScan::BestValue();

                if (liveScan > 0.0f)
                {
                    const float w = DlssNr::ExposureScan::AnchoredWhitePoint(
                        liveScan, config->DlssNrScanInverted.value_or_default(),
                        config->DlssNrScanTrim.value_or_default());

                    ImGui::TextDisabled("Scan %.5f  ->  white point %.2f   (%u point%s)", liveScan, w,
                                        (unsigned) anchors.size(), anchors.size() == 1 ? "" : "s");
                }
            }

            // Paper white shows only when there is a point to set: before the first anchor, or when a
            // row is selected to edit. Once points exist and none is selected, the white point is fixed
            // by the anchors and only the trim adjusts the live picture -- so the trim takes the
            // slider's place, the same shape as the game-exposure source.
            const bool showPaperWhite = anchors.empty() || editingRow;

            if (showPaperWhite)
            {
                float pw =
                    editingRow ? anchors[selectedAnchor].white : config->DlssNrWhitePointScale.value_or_default();

                char lbl[48];
                if (editingRow)
                    snprintf(lbl, sizeof(lbl), "Paper white (editing point %d)", selectedAnchor + 1);
                else
                    snprintf(lbl, sizeof(lbl), "Paper white");

                if (ImGui::SliderFloat(lbl, &pw, 0.25f, 2000.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
                {
                    if (editingRow)
                    {
                        DlssNr::ExposureScan::AnchorSetWhite(selectedAnchor, pw);
                        config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                    }
                    else
                        config->DlssNrWhitePointScale = pw;
                }

                HelpMarker("Adjust the selected calibration point, or set the value for the next point.\nUse Anchor "
                           "here to save the current lighting condition.");
            }

            // The trim multiplies the interpolated result, and in the steady state it is the control
            // that stands in for paper white: adjust it until the picture looks right in the current
            // light, then Anchor bakes that trimmed value into a new point and resets the trim to 1.
            if (!anchors.empty())
            {
                float trim = config->DlssNrScanTrim.value_or_default();

                if (ImGui::SliderFloat("Trim (x the scan)", &trim, 0.25f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
                    config->DlssNrScanTrim = std::clamp(trim, 0.25f, 4.0f);

                ImGui::SameLine();

                if (ImGui::SmallButton("Reset##scantrim"))
                    config->DlssNrScanTrim = 1.0f;

                HelpMarker("Multiply the calibrated white point. Anchor here saves the adjusted value and resets this "
                           "multiplier to 1.");
            }
        }
        else if (wpSource == 1)
        {
            const bool ofScan = false;

            float trim =
                ofScan ? config->DlssNrScanTrim.value_or_default() : config->DlssNrWhitePointTrim.value_or_default();

            if (ImGui::SliderFloat(ofScan ? "Trim (x the scan)" : "Trim (x the game's exposure)", &trim, 0.25f, 4.0f,
                                   "%.2fx", ImGuiSliderFlags_Logarithmic))
            {
                if (ofScan)
                    config->DlssNrScanTrim = std::clamp(trim, 0.25f, 4.0f);
                else
                    config->DlssNrWhitePointTrim = std::clamp(trim, 0.25f, 4.0f);
            }

            ImGui::SameLine();

            // Deliberately always present rather than greyed at 1. The point of it is that the safe
            // value is one click away without having to know what the safe value is.
            if (ImGui::SmallButton("Reset##wptrim"))
            {
                if (ofScan)
                    config->DlssNrScanTrim = 1.0f;
                else
                    config->DlssNrWhitePointTrim = 1.0f;
            }

            HelpMarker("Multiply the white point derived from game exposure. 1 = no adjustment.");
        }
        else
        {
            // Logarithmic, because the useful range is not linear. A quarter to 2000: the low end
            // because a frame the game already tone mapped wants roughly 1, the high end because
            // there is no principled ceiling -- this is a divisor on an open-ended linear buffer, and
            // how far up a given game needs to go is a property of that game's exposure rather than
            // of anything that can be bounded here. One tester was still improving at 100.
            float wpScale = config->DlssNrWhitePointScale.value_or_default();

            if (ImGui::SliderFloat("Paper white", &wpScale, 0.25f, 2000.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
                config->DlssNrWhitePointScale = wpScale;

            HelpMarker("Brightness reference used to prepare HDR colour for NR. Higher values darken the model input; "
                       "lower values brighten it.\nAdjust if NR loses detail or produces colour shifts.");
        }

        // Directly under the white point, because that is the number it moves and the number the
        // anchor captures. It used to sit under Inspect, a whole section away from the slider it
        // reads, which left "Anchor here" looking like a control for something else entirely.
        {
            // No checkbox here any more.
            //
            // The dropdown above says whether the scan is the white point's source, and that is
            // the only reason anybody using this would want it running. A second control could
            // only agree with the dropdown or contradict it, and both were on offer: it began as
            // a redundant question and became a way to switch off the thing the chosen source
            // depended on.
            //
            // The ini key survives as a developer override for the one case a user has no reason
            // to want -- running the scan in a game that supplies a REAL exposure, so the log can
            // compare the two. That is validation, and validation does not need a widget.
            //
            // Worth keeping written down, since the panel no longer says it: the scan matches
            // buffers by SHAPE, and shape is a weak filter. In GTA V -- a game that supplies a
            // real exposure, so the right answer sat visible beside it -- the best candidate was
            // a 1x1 R32_FLOAT that climbed in a straight line for seventeen minutes while the
            // true exposure held still. Their ratio moved 14x. That is an accumulator, not an
            // eye adaptation.

            // Only where it means something. The lamp reads the scan, so offering it beside a
            // white point that comes from the game's own exposure is offering a control that
            // cannot light up.
            bool meter = config->DlssNrScanMeter.value_or_default();

            if (config->DlssNrWhitePointSource.value_or_default() == 2 &&
                ImGui::Checkbox("Show exposure meter", &meter))
                config->DlssNrScanMeter = meter;

            HelpMarker(
                "Show the scanned exposure value and a colour indicator. Display only; does not change the image.");

            // Shown when the scan is actually running, whichever way it got switched on.
            if (DlssNr::ExposureScan::Scanning())
            {
                // Anchoring: one press, then it never needs touching again.
                //
                // The absolute white point cannot come out of a buffer whose units are unknown.
                // Every value AFTER the first can: only the ratio against the anchor is used, so
                // whatever the number means, it cancels. That is why this is a button and not a
                // measurement -- the one thing a person can supply that no amount of cleverness
                // can is "this looks right to me".
                int which = 0;
                float low = 0.0f, high = 0.0f;
                const float live = DlssNr::ExposureScan::BestValue(&which, &low, &high);

                const bool isSource = config->DlssNrWhitePointSource.value_or_default() == 2;

                // Anchor captures (currentScan, currentPaperWhite) and ADDS a row -- it does not
                // replace. One row is the old single-anchor ratio law; add a second in different
                // light and the white point is interpolated between the points, so it holds across
                // the whole range instead of only near one anchor. Greyed unless the scan is the
                // chosen source and it currently has a value to capture.
                ImGui::BeginDisabled(live <= 0.0f || !isSource);

                if (ImGui::Button("Anchor here"))
                {
                    // What to capture. Before the first point, the paper white above (an absolute value
                    // with the wide range a fresh game needs). After that, the EFFECTIVE white point the
                    // picture is showing right now -- the interpolated value times the Trim the user just
                    // dialed in -- so a second point in different light captures the trimmed look, not a
                    // frozen paper white (which would make two equal whites and a flat, non-tracking
                    // curve). The trim is reset afterwards: the new point, which the picture now passes
                    // through exactly, must not be multiplied by it a second time.
                    const float captureWhite =
                        anchors.empty() ? std::max(0.01f, config->DlssNrWhitePointScale.value_or_default())
                                        : std::max(0.01f, DlssNr::ExposureScan::AnchoredWhitePoint(
                                                              live, config->DlssNrScanInverted.value_or_default(),
                                                              config->DlssNrScanTrim.value_or_default()));

                    if (DlssNr::ExposureScan::AnchorAdd(live, captureWhite))
                    {
                        config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                        config->DlssNrScanTrim = 1.0f;
                        selectedAnchor = -1;
                    }
                }

                ImGui::EndDisabled();

                HelpMarker("Save the current exposure and white point as a calibration point.\nAdjust Paper white for "
                           "the first point, then Trim for additional lighting conditions. Up to 8 points.");

                if (!isSource)
                    ImGui::TextDisabled("Scanned exposure is not the selected white point source.");

                if (!anchors.empty())
                {
                    // The row nearest the live scan value (in log space) is the one driving the
                    // picture right now; mark it so the user can see which calibration is in effect.
                    int active = 0;
                    float bestDist = 1e30f;
                    const float liveLog = std::log(std::max(live, 1e-6f));

                    for (size_t i = 0; i < anchors.size(); ++i)
                    {
                        const float d = std::fabs(std::log(std::max(anchors[i].scan, 1e-6f)) - liveLog);
                        if (d < bestDist)
                        {
                            bestDist = d;
                            active = (int) i;
                        }
                    }

                    for (size_t i = 0; i < anchors.size(); ++i)
                    {
                        ImGui::PushID((int) i);

                        // Delete first, so its click is never swallowed by the row-wide Selectable.
                        if (ImGui::SmallButton("x"))
                        {
                            DlssNr::ExposureScan::AnchorRemove((int) i);
                            config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                            if (selectedAnchor == (int) i)
                                selectedAnchor = -1;
                            else if (selectedAnchor > (int) i)
                                --selectedAnchor;
                            ImGui::PopID();
                            continue;
                        }

                        ImGui::SameLine();

                        const bool sel = (int) i == selectedAnchor;
                        char row[96];
                        snprintf(row, sizeof(row), "%s scan %.4f  ->  white %.2f%s",
                                 ((int) i == active && isSource) ? ">" : "  ", anchors[i].scan, anchors[i].white,
                                 sel ? "   [editing]" : "");

                        // Click selects the row (slider edits it); click again deselects (slider
                        // returns to the live unanchored point).
                        if (ImGui::Selectable(row, sel))
                            selectedAnchor = sel ? -1 : (int) i;

                        ImGui::PopID();
                    }

                    ImGui::TextDisabled("Select a row to edit it; select it again"
                                        " to deselect. > marks the active point.");
                }

                // The direction flag only means anything with a single point; with two or more the
                // direction the white point moves is already fixed by the data.
                if (anchors.size() == 1)
                {
                    bool inverted = config->DlssNrScanInverted.value_or_default();
                    if (ImGui::Checkbox("Invert exposure tracking", &inverted))
                        config->DlssNrScanInverted = inverted;

                    HelpMarker("Reverse how scanned exposure changes the white point. Only needed with one calibration "
                               "point.");
                }

                // The scan -> white point readout is shown above the sliders now, not here.

                // Everything below is read-out rather than control: what the scan is looking at and
                // how to tell whether it found the right thing. Folded away because the two decisions
                // that matter -- anchor, and which way the number runs -- are above it.
                if (ImGui::TreeNode("Advanced"))
                {

                    const auto found = DlssNr::ExposureScan::Report();
                    const char* why = DlssNr::ExposureScan::Status();

                    if (found.empty())
                    {
                        ImGui::TextDisabled("%s",
                                            why != nullptr && why[0] != 0 ? why : "No exposure candidates found.");
                    }
                    else
                    {
                        for (size_t i = 0; i < found.size(); ++i)
                        {
                            const auto& c = found[i];

                            if (c.reads == 0)
                            {
                                ImGui::TextDisabled("%zu. %s -- not read yet", i + 1, c.shape.c_str());
                                continue;
                            }

                            // Keep moving candidates readable and dim inactive ones using the HDR-adjusted theme.
                            ImGui::TextColored(
                                ImGui::GetStyleColorVec4(c.moves ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                                "%zu. %s = %.5f  (seen %.5f..%.5f) %s", i + 1, c.shape.c_str(), c.latest, c.lowest,
                                c.highest, c.moves ? "MOVES" : "flat so far");
                        }

                        ImGui::TextDisabled("Move between bright and dark areas to check exposure tracking.");
                        ImGui::TextDisabled("A value that only increases may be a counter.");
                    }

                    ImGui::TreePop();
                }
            }
        }
    }
}

static void RenderModel(Config* config, float menuResScale)
{
    // Keep the UI simple; advanced INI pass settings remain available.
    constexpr int menuPassLimit = 2;
    {
        int passes = (int) std::clamp(config->DlssNrPasses.value_or_default(), 1u, (unsigned int) menuPassLimit);
        if (ImGui::SliderInt("Model passes", &passes, 1, menuPassLimit, passes == 1 ? "%d (normal)" : "%dx model cost",
                             ImGuiSliderFlags_AlwaysClamp))
            config->DlssNrPasses = (uint32_t) std::clamp(passes, 1, menuPassLimit);

        HelpMarker("Choose 1 or 2 model passes. A second pass strengthens the effect and increases GPU cost.\nEach "
                   "pass has its own settings and history. Start with 1.");
    }

    // Any percentage, rather than a handful of steps somebody chose in advance. The lower bound
    ImGui::TextWrapped("Settings apply when you release a slider.");
    static const char* styles[] = { "Standard", "Natural", "Cinematic" };
    static const char* inheritedStyles[] = { "Auto (inherit pass 1)", "Standard", "Natural", "Cinematic" };

    if (ImGui::TreeNodeEx("Pass 1", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int style = (int) std::min(config->DlssNrStyle.value_or_default(), 2u);
        if (ImGui::Combo("Style", &style, styles, IM_ARRAYSIZE(styles)))
            config->DlssNrStyle = (uint32_t) style;
        HelpMarker("Select the appearance profile: Standard, Natural or Cinematic. Intensity controls its strength.");
        DeferredSlider("Intensity", &config->DlssNrIntensity, 0.0f, 2.0f, 1.0f);
        DeferredSlider("Local structure", &config->DlssNrLocalStructure, 0.0f, 2.0f, 1.0f);
        DeferredSlider("Local tone", &config->DlssNrLocalTone, 0.0f, 2.0f, 1.0f);
        DeferredSlider("Skin structure", &config->DlssNrSkinStructure, -1.0f, 2.0f, -1.0f);
        bool mask = config->DlssNrAutoMask.value_or_default();
        if (ImGui::Checkbox("Auto skin mask", &mask))
            config->DlssNrAutoMask = mask;
        HelpMarker("Use the model's learned skin selection to apply Skin structure without an authored mask.\nAccuracy "
                   "varies. This is separate from the colour-based mask below.");
        ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Pass 2", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped("Defaults: inherit Pass 1; Local tone = 0. Reset restores these defaults.");
        InheritedProfileCombo("Style", &config->DlssNrPass2Style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
        DeferredSlider("Intensity", &config->DlssNrPass2Intensity, 0.0f, 2.0f,
                       config->DlssNrIntensity.value_or_default(), "%.2f", true);
        DeferredSlider("Local structure", &config->DlssNrPass2LocalStructure, 0.0f, 2.0f,
                       config->DlssNrLocalStructure.value_or_default(), "%.2f", true);
        DeferredSlider("Local tone", &config->DlssNrPass2LocalTone, 0.0f, 2.0f, 0.0f, "%.2f", true);
        DeferredSlider("Skin structure", &config->DlssNrPass2SkinStructure, -1.0f, 2.0f,
                       config->DlssNrSkinStructure.value_or_default(), "%.2f", true);
        bool mask = config->DlssNrPass2AutoMask.has_value() ? config->DlssNrPass2AutoMask.value()
                                                            : config->DlssNrAutoMask.value_or_default();
        if (ImGui::Checkbox("Auto skin mask", &mask))
            config->DlssNrPass2AutoMask = mask;
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##mask"))
            config->DlssNrPass2AutoMask = std::optional<bool> {};
        ImGui::TreePop();
    }

    ImGui::TextWrapped("Pass settings apply to SR and RR on DX12 and native Vulkan.");
}

static void RenderBlend(Config* config, float menuResScale)
{
    bool applyModel = config->DlssNrApplyModel.value_or_default();
    if (ImGui::Checkbox("Apply the model", &applyModel))
        config->DlssNrApplyModel = applyModel;

    HelpMarker("Show or hide the NR effect. The model still runs when hidden.\nDisable Enable Neural Rendering to stop "
               "its GPU cost.");

    float transfer = config->DlssNrTransferStrength.value_or_default();
    if (ImGui::SliderFloat("Detail strength", &transfer, 0.0f, 2.0f, "%.2f"))
        config->DlssNrTransferStrength = transfer;

    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##detail"))
        config->DlssNrTransferStrength = 1.0f;

    HelpMarker("Overall NR detail strength: 0 = no effect, 1 = normal, above 1 = exaggerated.");

    float colour = config->DlssNrColourStrength.value_or_default();
    if (ImGui::SliderFloat("Colour strength", &colour, 0.0f, 4.0f, "%.2f"))
        config->DlssNrColourStrength = colour;

    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##colour"))
        config->DlssNrColourStrength = 1.0f;

    HelpMarker("NR colour strength: 0 = preserve game colours, 1 = model colours, above 1 = stronger saturation.");

    if (ImGui::TreeNode("Skin and environment (final edit)"))
    {
        ImGui::TextWrapped("Select skin by colour and adjust the final NR effect separately for skin and scenery. "
                           "Selection can be inaccurate; check Preview.");
        bool filter = config->DlssNrSkinProtection.value_or_default();
        if (ImGui::Checkbox("Separate skin / environment controls", &filter))
            config->DlssNrSkinProtection = filter;
        ImGui::BeginDisabled(!filter);
        bool tone = config->DlssNrSkinToneEnabled.value_or_default();
        if (ImGui::Checkbox("Allow skin tone / colour changes", &tone))
            config->DlssNrSkinToneEnabled = tone;
        HelpMarker("Allow NR colour changes in the selected skin region. Turn off to preserve its colour; detail can "
                   "still change.");
        const auto slider = [](const char* label, auto& option)
        {
            float v = option.value_or_default();
            if (ImGui::SliderFloat(label, &v, 0.0f, 1.0f, "%.2f"))
                option = v;
            HelpMarker("NR strength in this region: 0 = no change, 1 = full effect.");
        };
        slider("Skin detail / lighting", config->DlssNrSkinDetail);
        ImGui::BeginDisabled(!tone);
        slider("Skin colour", config->DlssNrSkinColour);
        ImGui::EndDisabled();
        slider("Environment detail / lighting", config->DlssNrEnvironmentDetail);
        slider("Environment colour", config->DlssNrEnvironmentColour);
        bool preview = config->DlssNrShowSkinMask.value_or_default();
        if (ImGui::Checkbox("Preview colour-based mask", &preview))
            config->DlssNrShowSkinMask = preview;
        ImGui::EndDisabled();
        ImGui::TreePop();
    }

    // Highlight guard, directly under the white point / trim -- it bounds the model's edit and
    // belongs with the exposure controls it works alongside.
    float maxRatio = config->DlssNrMaxRatio.value_or_default();
    if (ImGui::SliderFloat("Highlight guard", &maxRatio, 1.0f, 8.0f, "%.1fx"))
        config->DlssNrMaxRatio = maxRatio;

    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##guard"))
        config->DlssNrMaxRatio = 2.0f;

    HelpMarker("Limit how much NR can brighten or darken a pixel. Lower values restrict highlight changes; higher "
               "values allow more.");
}

static void RenderInspect(Config* config, float menuResScale)
{
    bool held = config->DlssNrHoldFrame.value_or_default();
    if (ImGui::Checkbox("Hold frame", &held))
        config->DlssNrHoldFrame = held;

    HelpMarker("Freeze NR's input to compare its settings. The game's HUD and later effects may keep updating.\nDoes "
               "not re-run SR/RR or show changes to their settings. Turn off to resume.");

    static const char* compareNames[] = { "Off", "Side by side", "Wipe" };
    int compare = (int) config->DlssNrCompare.value_or_default();
    if (ImGui::Combo("Compare", &compare, compareNames, IM_ARRAYSIZE(compareNames)))
        config->DlssNrCompare = (uint32_t) compare;

    HelpMarker("Compare the original and NR result. Side by side fits both images; Wipe divides one full-size image.");

    if (compare != 0)
    {
        bool swap = config->DlssNrCompareSwap.value_or_default();
        if (ImGui::Checkbox("Swap sides", &swap))
            config->DlssNrCompareSwap = swap;
        HelpMarker("Swap the original and NR sides.");

        bool tags = config->DlssNrCompareTags.value_or_default();
        if (ImGui::Checkbox("Label the sides", &tags))
            config->DlssNrCompareTags = tags;

        HelpMarker("Display labels identifying the original and NR sides.");

        if (tags)
        {
            float tagScale = config->DlssNrTagScale.value_or_default();
            if (ImGui::SliderFloat("Label size", &tagScale, 0.5f, 5.0f, "%.1fx"))
                config->DlssNrTagScale = std::clamp(tagScale, 0.5f, 5.0f);
        }
    }

    if (compare == 1)
    {
        float zoom = config->DlssNrCompareZoom.value_or_default();
        if (ImGui::SliderFloat("Zoom", &zoom, 1.0f, 2.0f, "%.2f"))
            config->DlssNrCompareZoom = std::clamp(zoom, 1.0f, 2.0f);

        HelpMarker("Side-by-side zoom: 1 = fit the whole image, 2 = fill each half by cropping the sides.");
    }

    if (compare == 2)
    {
        float split = config->DlssNrCompareSplit.value_or_default();
        if (ImGui::SliderFloat("Split", &split, 0.0f, 1.0f, "%.2f"))
            config->DlssNrCompareSplit = std::clamp(split, 0.0f, 1.0f);

        HelpMarker("Position of the comparison boundary. Swap sides reverses which image appears on each side.");
    }

    static const char* debugNames[] = { "Off", "Proxy (what the model sees)", "Model output (raw)",
                                        "Difference (amplified)" };
    int debugView = (int) config->DlssNrDebugView.value_or_default();
    if (ImGui::Combo("Debug view", &debugView, debugNames, IM_ARRAYSIZE(debugNames)))
        config->DlssNrDebugView = (uint32_t) debugView;

    HelpMarker("Show the model input, raw output, or a 20x amplified difference. Grey in Difference means no change.");
}

void RenderMenu(Config* config, float menuResScale)
{
    ImGui::Spacing();
    if (auto header = ScopedCollapsingHeader("DLSS Neural Rendering"); header.IsHeaderOpen())
    {
        ScopedIndent indent {};
        bool enabled = config->DlssNrEnabled.value_or_default();
        if (ImGui::Checkbox("Enable Neural Rendering", &enabled))
            config->DlssNrEnabled = enabled;
        HelpMarker("Requires nvngx_dlssnr.dll and a compatible NVIDIA driver. Disabling NR stops its GPU work.");

        PipelineUi::View view;
        view.enabled = enabled;
        view.applyModel = config->DlssNrApplyModel.value_or_default();
        view.passes = config->DlssNrPasses.value_or_default();
        view.scalePercent = (int) lroundf(config->DlssNrWorkingScale.value_or_default() * 100.0f);
        const auto feature = State::Instance().currentFeature;
        view.rayReconstruction = feature && feature->GetUpscalerType() == Upscaler::DLSSD;
        const bool finished = config->DlssNrFinishedPicture.value_or_default();
        const bool before = config->DlssNrRunBeforeSr.value_or_default();
        const bool deferred = config->DlssNrDeferredDlss.value_or_default();
        if (finished)
            view.route = before || deferred ? PipelineUi::Route::FinishedBefore : PipelineUi::Route::Finished;
        else if (deferred && !view.rayReconstruction)
            view.route = PipelineUi::Route::Deferred;
        else if (before && view.rayReconstruction && config->DlssNrResidualAcrossRr.value_or_default())
            view.route = PipelineUi::Route::AcrossRr;
        else
            view.route = before ? PipelineUi::Route::Before : PipelineUi::Route::After;

        static PipelineUi::Section selected = PipelineUi::Section::Placement;
        PipelineUi::Draw(view, selected);
        ImGui::Spacing();
        RenderStatus(config, menuResScale);
        ImGui::SeparatorText(PipelineUi::SectionName(selected));
        ImGui::PushItemWidth(std::min(220.0f * menuResScale, ImGui::GetContentRegionAvail().x * 0.42f));
        switch (selected)
        {
        case PipelineUi::Section::Placement:
            RenderPlacement(config, menuResScale);
            break;
        case PipelineUi::Section::Input:
            RenderInput(config, menuResScale);
            break;
        case PipelineUi::Section::Model:
            RenderModel(config, menuResScale);
            break;
        case PipelineUi::Section::Blend:
            RenderBlend(config, menuResScale);
            break;
        case PipelineUi::Section::Inspect:
            RenderInspect(config, menuResScale);
            break;
        }
        ImGui::PopItemWidth();
    }
}

} // namespace DlssNr
