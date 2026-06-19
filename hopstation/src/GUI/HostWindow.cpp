#include "HostWindow.h"

#include "AudioDevice.h"
#include "Host.h"

#include "core/Log.h"

#include "imgui.h"

bool HostWindow::s_visible = false;

void HostWindow::Update()
{
	if (!s_visible)
		return;

	if (ImGui::Begin("Host", &s_visible))
	{
		unsigned int queueSizeFrames = AudioDevice::GetQueueSizeFrames();
		ImGui::Text("Audio device queue size, frames: %u", queueSizeFrames);

		bool dynamicAudioResamplingEnabled = Host::IsDynamicAudioResamplingEnabled();
		if (ImGui::Checkbox("Dynamic audio resampling", &dynamicAudioResamplingEnabled))
			Host::SetDynamicAudioResamplingEnabled(dynamicAudioResamplingEnabled);

		ImGui::Text("Current audio resampling frequency ratio: %.4f", Host::GetCurrentAudioResamplingFrequencyRatio());

		ImGui::End();
	}
}
