#pragma once

#include <imgui/imgui.h>
#include <algorithm>
#include <string>

namespace DlssNr::PipelineUi
{
enum class Section
{
    Placement,
    Input,
    Model,
    Blend,
    Inspect
};
enum class Route
{
    Before,
    After,
    Deferred,
    AcrossRr,
    Finished,
    FinishedBefore
};

struct View
{
    Route route = Route::After;
    bool enabled = true;
    bool rayReconstruction = false;
    bool applyModel = true;
    unsigned int passes = 1;
    int scalePercent = 100;
};

inline const char* SectionName(Section section)
{
    switch (section)
    {
    case Section::Placement:
        return "Placement - choose where NR runs";
    case Section::Input:
        return "Prepare input - resolution, HDR and exposure";
    case Section::Model:
        return "Model passes - generate the edit";
    case Section::Blend:
        return "Apply edit - strength, skin and highlights";
    case Section::Inspect:
        return "Inspect NR - hold, compare and debug";
    }
    return "";
}

// This describes the configured colour/edit flow. It never changes a rendering option.
inline void Draw(const View& view, Section& selected)
{
    ImGui::PushID("NR pipeline chart");
    ImGui::TextUnformatted("Configured render pipeline");
    ImGui::TextWrapped("Select a box to edit its settings. Game stages are shown for context.");
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize(), 260.0f);
    const float gap = ImGui::GetFontSize() * 2.5f;
    const float nodeWidth = (width - gap) * 0.5f;
    const float height = ImGui::GetTextLineHeight() * 2.0f + ImGui::GetStyle().FramePadding.y * 2.0f + 8.0f;
    const float step = height + ImGui::GetFontSize() * 1.15f;
    struct Node
    {
        int column, row;
        const char* title;
        std::string detail;
        int section;
        bool nr;
    };
    const bool split =
        view.route == Route::Deferred || view.route == Route::AcrossRr || view.route == Route::FinishedBefore;
    const bool before = view.route == Route::Before || split;
    const bool finished = view.route == Route::Finished || view.route == Route::FinishedBefore;
    const int prepareRow = view.route == Route::Finished ? 3 : before ? 1 : 2;
    Node nodes[] {
        { 0, 0, "Game input", "Placement / routing", (int) Section::Placement, false },
        { 1, prepareRow, "Prepare NR input", "HDR / exposure / " + std::to_string(view.scalePercent) + "%",
          (int) Section::Input, true },
        { 1, prepareRow + 1, "NR model",
          std::to_string(view.passes) + (view.passes == 1 ? " pass / tuning" : " passes / tuning"),
          (int) Section::Model, true },
        { 1,
          finished ? 5
          : split  ? 4
                   : prepareRow + 2,
          "Apply NR edit", view.applyModel ? "Strength / skin" : "Edit hidden; model runs",
          (int) Section::Blend, true },
        { 0, before ? 4 : 1, view.rayReconstruction ? "RR + Super Resolution" : "Super Resolution", "Game upscaler",
          -1, false },
        { 0, view.route == Route::Finished ? 2 : 5, "Game effects + HUD", "Game rendering", -1, false },
        { 0, 6, "Game output", "FG / presentation", -1, false },
        { 1, 6, "Inspect NR", "Hold / compare / debug", (int) Section::Inspect, true },
        { 1, 3, view.route == Route::AcrossRr ? "Accumulate NR edit" : "Upscale NR edit",
          view.route == Route::AcrossRr ? "Motion-guided residual" : "Separate DLSS pass", -1, true }
    };
    const auto topLeft = [&](int i)
    { return ImVec2(origin.x + nodes[i].column * (nodeWidth + gap), origin.y + nodes[i].row * step); };
    auto* draw = ImGui::GetWindowDrawList();
    const ImU32 lineColour = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const auto connect = [&](int from, int to, bool tap = false)
    {
        const auto a = topLeft(from), b = topLeft(to);
        ImVec2 start(a.x + nodeWidth * 0.5f, a.y + height);
        ImVec2 end(b.x + nodeWidth * 0.5f, b.y);
        if (nodes[from].row == nodes[to].row)
        {
            start = ImVec2(a.x + nodeWidth, a.y + height * 0.5f);
            end = ImVec2(b.x, b.y + height * 0.5f);
            draw->AddLine(start, end, lineColour, 1.5f);
            draw->AddTriangleFilled(end, ImVec2(end.x - 5, end.y - 3), ImVec2(end.x - 5, end.y + 3), lineColour);
            return;
        }
        const float mid = (start.y + end.y) * 0.5f;
        // Inspection is a side tap, not another rendering pass.
        if (tap)
        {
            const float x = a.x + nodeWidth + gap * 0.25f;
            start = ImVec2(a.x + nodeWidth, a.y + height * 0.5f);
            end = ImVec2(b.x + nodeWidth, b.y + height * 0.5f);
            draw->AddLine(start, ImVec2(x, start.y), lineColour);
            for (float y = start.y; y < end.y; y += 8.0f)
                draw->AddLine(ImVec2(x, y), ImVec2(x, std::min(y + 4.0f, end.y)), lineColour);
            draw->AddLine(ImVec2(x, end.y), end, lineColour);
        }
        else
        {
            draw->AddLine(start, ImVec2(start.x, mid), lineColour, 1.5f);
            draw->AddLine(ImVec2(start.x, mid), ImVec2(end.x, mid), lineColour, 1.5f);
            draw->AddLine(ImVec2(end.x, mid), end, lineColour, 1.5f);
            draw->AddTriangleFilled(end, ImVec2(end.x - 3, end.y - 5), ImVec2(end.x + 3, end.y - 5), lineColour);
        }
    };
    if (!view.enabled)
    {
        connect(0, 4);
        connect(4, 5);
        connect(5, 6);
    }
    else
    {
        if (split)
        {
            connect(0, 4);
            connect(0, 1);
            connect(1, 2);
            connect(2, 8);
            connect(8, 3);
            if (finished)
            {
                connect(4, 5);
                connect(5, 3);
                connect(3, 6);
            }
            else
            {
                connect(4, 3);
                connect(3, 5);
                connect(5, 6);
            }
        }
        else if (before)
        {
            connect(0, 1);
            connect(1, 2);
            connect(2, 3);
            connect(3, 4);
            connect(4, 5);
            connect(5, 6);
        }
        else if (finished)
        {
            connect(0, 4);
            connect(4, 5);
            connect(5, 1);
            connect(1, 2);
            connect(2, 3);
            connect(3, 6);
        }
        else
        {
            connect(0, 4);
            connect(4, 1);
            connect(1, 2);
            connect(2, 3);
            connect(3, 5);
            connect(5, 6);
        }
        connect(3, 7, true);
    }
    for (int i = 0; i < (split ? 9 : 8); ++i)
    {
        const auto& node = nodes[i];
        ImGui::SetCursorScreenPos(topLeft(i));
        ImGui::PushID(i);
        const bool chosen = node.section >= 0 && node.section == (int) selected;
        if (chosen)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        if (node.nr && !view.enabled)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.55f);
        ImGui::BeginDisabled(node.section < 0);
        if (ImGui::Button((std::string(node.title) + "\n" + node.detail).c_str(), ImVec2(nodeWidth, height)))
            selected = (Section) node.section;
        ImGui::EndDisabled();
        if (node.nr && !view.enabled)
            ImGui::PopStyleVar();
        if (chosen)
            ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 7 * step));
    ImGui::Dummy(ImVec2(width, 0));
    ImGui::TextDisabled("Inspection taps the NR boundary. Later game effects may change the image.");
    ImGui::PopID();
}
} // namespace DlssNr::PipelineUi
