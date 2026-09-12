#include "pch.h"

#include "DlssNr.h"
#include "DlssNr_PipelineUi.h"
#include "DlssNr_Upscaler.h"
#include "DlssNr_MenuSections.h"
#include <Config.h>
#include <menu/menu_common.h>
#include <algorithm>
#include <cmath>

namespace DlssNr
{

void RenderMenu(Config* config, float menuResScale)
{
    using namespace MenuSections;
    ImGui::Spacing();
    if (auto header = ScopedCollapsingHeader("DLSS Neural Rendering"); header.IsHeaderOpen())
    {
        ScopedIndent indent {};
        const float toggleGap = ImGui::GetStyle().ItemSpacing.x;
        const float toggleWidth = (ImGui::GetContentRegionAvail().x - toggleGap) * 0.5f;
        const float toggleRight = ImGui::GetCursorPosX() + toggleWidth + toggleGap;
        bool enabled = config->DlssNrEnabled.value_or_default();
        if (PipelineUi::CheckboxWrapped("Enable Neural Rendering", &enabled, toggleWidth))
            config->DlssNrEnabled = enabled;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Enable NR processing.");

        bool applyModel = config->DlssNrApplyModel.value_or_default();
        if (PipelineUi::CheckboxWrapped("Apply model", &applyModel, toggleWidth))
            config->DlssNrApplyModel = applyModel;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show or hide the NR effect. The model still runs when hidden.\nDisable Enable Neural "
                              "Rendering to stop its GPU cost.");

        const auto feature = State::Instance().currentFeature;
        const bool rayReconstruction = feature && feature->GetUpscalerType() == Upscaler::DLSSD;
        bool finished = config->DlssNrFinishedPicture.value_or_default();
        const bool deferredActive = !finished && config->DlssNrDeferredDlss.value_or_default() && !rayReconstruction;
        bool generateBefore = config->DlssNrRunBeforeSr.value_or_default() ||
                              (finished && config->DlssNrDeferredDlss.value_or_default()) || deferredActive;
        ImGui::SameLine(toggleRight);
        ImGui::BeginDisabled(deferredActive);
        if (PipelineUi::CheckboxWrapped("Generate model before upscale", &generateBefore, toggleWidth))
        {
            config->DlssNrRunBeforeSr = generateBefore;
            if (finished)
                config->DlssNrDeferredDlss = false;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(deferredActive ? "This mode always generates before upscale."
                              : finished ? "Generate early; apply the edit to the finished frame."
                                         : "Run NR before the upscaler.");

        if (PipelineUi::CheckboxWrapped("Apply NR to the finished picture", &finished, toggleWidth))
        {
            config->DlssNrFinishedPicture = finished;
            DlssNr::RetryAfterFailure();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Apply NR after game effects and HUD.");

        ImGui::SameLine(toggleRight);
        bool deferredDlss = config->DlssNrDeferredDlss.value_or_default();
        ImGui::BeginDisabled(finished);
        if (PipelineUi::CheckboxWrapped("Generate before SR, apply after SR", &deferredDlss, toggleWidth))
            config->DlssNrDeferredDlss = deferredDlss;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(finished ? "Use Generate model before upscale."
                                      : "Generate early, upscale the edit with the private backend, then apply it after SR.");
        ImGui::Spacing();

        const auto privateFeature = State::Instance().currentFeature;
        const bool nativePrivateVk =
            privateFeature && privateFeature->Api() == API::Vulkan && !privateFeature->IsWithDx12();
        if (!rayReconstruction && !nativePrivateVk &&
            (deferredDlss || (finished && config->DlssNrRunBeforeSr.value_or_default())))
        {
            int backend = (int) GetPrivateUpscaler(config->DlssNrPrivateUpscaler.value_or_default());
            if (ImGui::Combo("Private NR upscaler", &backend, "DLSS\0FSR 2.2\0FSR (FidelityFX)\0XeSS\0"))
                config->DlssNrPrivateUpscaler = backend;
            HelpMarker("Upscales the NR edit. FSR (FidelityFX) and XeSS need their runtimes.");
        }

        PipelineUi::View view;
        view.privateUpscaler = PrivateUpscalerName(GetPrivateUpscaler(config->DlssNrPrivateUpscaler.value_or_default()));
        view.enabled = enabled;
        view.applyModel = config->DlssNrApplyModel.value_or_default();
        view.passes = config->DlssNrPasses.value_or_default();
        view.scalePercent = (int) lroundf(config->DlssNrWorkingScale.value_or_default() * 100.0f);
        view.rayReconstruction = rayReconstruction;
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
        ImGui::Separator();
        PipelineUi::Draw(view, selected);
        ImGui::Separator();
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
        }
        ImGui::PopItemWidth();
        if (ImGui::CollapsingHeader("Inspect NR"))
        {
            ImGui::PushItemWidth(std::min(220.0f * menuResScale, ImGui::GetContentRegionAvail().x * 0.42f));
            RenderInspect(config, menuResScale);
            ImGui::PopItemWidth();
        }
    }
}

} // namespace DlssNr
