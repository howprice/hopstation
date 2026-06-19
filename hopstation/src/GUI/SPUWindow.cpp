#include "SPUWindow.h"

#include "psx/SPU.h"

#include "core/StringHelpers.h"
#include "core/MathsHelpers.h"
#include "core/ArrayHelpers.h"

#include "ImGuiWrap.h"
#include "imgui.h"

bool SPUWindow::s_visible = false;

static void showSPUCNT(SPU& spu)
{
	SPU::SPUCNT& spucnt = spu.GetSPUCNT();
	if (ImGui::BeginTable("SPUCNTTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
	{
		ImGui::TableNextRow();
		if (spucnt.SPUEnable)
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(0, 255, 0, 50)); // semi-transparent green
		ImGui::TableNextColumn();
		ImGui::Text("SPU Enabled");
		ImGui::TableNextColumn();
		ImGui::Text("%u", spucnt.SPUEnable);

		ImGui::TableNextRow();
		if (spucnt.MuteSPU == 0) // active low
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(255, 0, 0, 50)); // semi-transparent red
		ImGui::TableNextColumn();
		ImGui::Text("Mute SPU");
		ImGui::TableNextColumn();
		ImGui::Text("%u (active low)", spucnt.MuteSPU);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Noise Frequency Shift");
		ImGui::TableNextColumn();
		ImGui::PushStyleCompact();
		int noiseFreqShift = (int)spucnt.NoiseFrequencyShift;
		if (ImGui::SliderInt("##NoiseFrequencyShift", &noiseFreqShift, 0, 15))
			spucnt.NoiseFrequencyShift = (u16)Clamp(noiseFreqShift, 0, 15);
		ImGui::PopStyleCompact();
		ImGui::SameLine();
		ImGui::Text("[0,F]");

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Noise Frequency Step");
		ImGui::TableNextColumn();
		ImGui::PushStyleCompact();
		int noiseFreqStep = (int)spucnt.NoiseFrequencyStep;
		if (ImGui::SliderInt("##NoiseFrequencyStep", &noiseFreqStep, 0, 3))
			spucnt.NoiseFrequencyStep = (u16)Clamp(noiseFreqStep, 0, 3);
		ImGui::PopStyleCompact();
		ImGui::SameLine();
		ImGui::Text("[0,3] = [4,7]");

		ImGui::TableNextRow();
		if (spucnt.ReverbMasterEnable)
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(0, 255, 0, 50)); // semi-transparent green
		ImGui::TableNextColumn();
		ImGui::Text("Reverb Master Enable");
		ImGui::TableNextColumn();
		ImGui::Text("%u", spucnt.ReverbMasterEnable);

		// IRQ9
		ImGui::TableNextRow();
		if (spucnt.IRQ9Enable)
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(0, 255, 0, 50)); // semi-transparent green
		ImGui::TableNextColumn();
		ImGui::Text("IRQ9 Enable");
		ImGui::TableNextColumn();
		ImGui::Text("%u address %08X", spucnt.IRQ9Enable, spu.GetSoundRAMIRQAddress());

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Sound RAM Transfer Mode");
		ImGui::TableNextColumn();
		ImGui::Text("%u %s", (int)spucnt.soundRAMTransferMode, kSoundRAMTransferModeNames[(int)spucnt.soundRAMTransferMode]);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("External Audio Reverb");
		ImGui::TableNextColumn();
		ImGui::Text("%u", spucnt.ExternalAudioReverb);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("CD Audio Reverb");
		ImGui::TableNextColumn();
		ImGui::Text("%u", spucnt.CDAudioReverb);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("External Audio Enable");
		ImGui::TableNextColumn();
		ImGui::Text("%u", spucnt.ExternalAudioEnable);

		ImGui::TableNextRow();
		if (spucnt.CDAudioEnable)
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(0, 255, 0, 50)); // semi-transparent green
		ImGui::TableNextColumn();
		ImGui::Text("CD Audio Enable");
		ImGui::TableNextColumn();
		ImGui::Text("%u", spucnt.CDAudioEnable);
		ImGui::EndTable();
	}
}

static void showVolumes(const SPU& spu)
{
	// Main volume
	u16 mainVolumeL = spu.GetMainVolumeLeft();
	if (mainVolumeL & 0x8000)
		ImGui::Text("Main Volume L: %04X SWEEP MODE NOT IMPLEMENTED", mainVolumeL);
	else
	{
		int volume = (int)(s16)(mainVolumeL << 1);
		ImGui::Text("Main Volume L: %04X = %s%04X", mainVolumeL, volume < 0 ? "-" : " ", volume < 0 ? -volume : volume);
	}

	ImGui::SameLine();
	u16 mainVolumeR = spu.GetMainVolumeRight();
	if (mainVolumeR & 0x8000)
		ImGui::Text("Main Volume R: %04X SWEEP MODE NOT IMPLEMENTED", mainVolumeR);
	else
	{
		int volume = (int)(s16)(mainVolumeR << 1);
		ImGui::Text("Main Volume R: %04X = %s%04X", mainVolumeR, volume < 0 ? "-" : " ", volume < 0 ? -volume : volume);
	}

	// CD volume
	s16 cdVolumeL = spu.GetCDVolumeLeft();
	s16 cdVolumeR = spu.GetCDVolumeRight();
	ImGui::Text("CD Volume L: %04X = %s%04X", cdVolumeL, cdVolumeL < 0 ? "-" : " ", cdVolumeL < 0 ? -(int)cdVolumeL : cdVolumeL);
	ImGui::SameLine();
	ImGui::Text("CD Volume R: %04X = %s%04X", cdVolumeR, cdVolumeR < 0 ? "-" : " ", cdVolumeR < 0 ? -(int)cdVolumeR : cdVolumeR);
}

static void showVoice(const SPU::Voice& voice)
{
	ImGui::Text("Volume Left: %04X", voice.volumeLeft);
	ImGui::Text("Volume Right: %04X", voice.volumeRight);
	ImGui::Text("ADPCM Sample Rate: %04X", voice.sampleRate);
	ImGui::Text("ADPCM Start Address: %04X = %08X", voice.startAddressDiv8, voice.startAddressDiv8 << 3);
	ImGui::Text("ADPCM Repeat Address: %04X = %08X", voice.repeatAddressDiv8, voice.repeatAddressDiv8 << 3);
	ImGui::Text("ADPCM Current Address: %08X", voice.currentADPCMAddress);
	ImGui::Text("Pitch counter: %08x", voice.pitchCounter);
	ImGui::Text("Sample index: %u", voice.currentSampleIndex);

	ImGui::Spacing();

	// ADSR envelope
	ImGui::Text("ADSR Phase: %s", kADSRPhaseNames[(int)voice.adsr.phase]);
	ImGui::Text("Current level: %d (decimal)", voice.currentADSREnvelopeLevel);
	ImGui::Text("Sustain Level: %u", voice.attackDecaySustainLevel.sustainLevel);
	ImGui::Text("Decay Shift: %u", voice.attackDecaySustainLevel.decayShift);
	ImGui::Text("Attack Step: %u", voice.attackDecaySustainLevel.attackStep);
	ImGui::Text("Attack Shift: %u", voice.attackDecaySustainLevel.attackShift);
	ImGui::Text("Attack Mode: %u", voice.attackDecaySustainLevel.attackMode);
	ImGui::Text("Release Shift: %u", voice.sustainReleaseRate.releaseShift);
	ImGui::Text("Release Mode: %u", voice.sustainReleaseRate.releaseMode);
	ImGui::Text("Sustain Step: %u", voice.sustainReleaseRate.sustainStep);
	ImGui::Text("Sustain Shift: %u", voice.sustainReleaseRate.sustainShift);
	ImGui::Text("Sustain Direction: %u", voice.sustainReleaseRate.sustainDirection);
	ImGui::Text("Sustain Mode: %u", voice.sustainReleaseRate.sustainMode);

	ImGui::Spacing();
	ImGui::Text("Counter: %u", voice.adsr.counter);
	ImGui::Text("Counter Increment: %u", voice.adsr.counterIncrement);
	ImGui::Text("Step: %d", voice.adsr.step);
	ImGui::Text("Decreasing: %u", voice.adsr.decreasing);
	ImGui::Text("Exponential: %u", voice.adsr.exponential);
	ImGui::Text("Phase Negative: %u", voice.adsr.phaseNegative);
}

static void showVoices(SPU& spu)
{
	if (ImGui::Button("Mute all"))
		spu.m_debugVoiceEnabled = 0;
	ImGui::SameLine();
	if (ImGui::Button("Unmute all"))
		spu.m_debugVoiceEnabled = 0xffffff; // 24 voices
	ImGui::SameLine();
	if (ImGui::Button("Invert mute"))
		spu.m_debugVoiceEnabled ^= 0xffffff; // 24 voices

	const SPU::VoiceFlags& voiceFlags = spu.GetVoiceFlags();

	for (unsigned int voiceIndex = 0; voiceIndex < SPU::kVoiceCount; voiceIndex++)
	{
		const SPU::Voice& voice = spu.GetVoice(voiceIndex);

		const unsigned int voiceFlag = 1 << voiceIndex;

		// Add mute checkbox before collapsing header
		char label[256];
		SafeSnprintf(label, sizeof(label), "##%u_enabled", voiceIndex);
		ImGui::CheckboxFlags(label, &spu.m_debugVoiceEnabled, voiceFlag);
		ImGui::SameLine();

		if (voice.adsr.phase == SPU::ADSRPhase::None)
			SafeSnprintf(label, sizeof(label), "Voice %02u", voiceIndex);
		else
			SafeSnprintf(label, sizeof(label), "Voice %02u %s StartAddress: %08X, SampleRate: %04X%s%s%s###Voice %02u",
				voiceIndex, kADSRPhaseNames[(int)voice.adsr.phase], voice.startAddressDiv8 * 8, voice.sampleRate,
				(voiceFlags.pmon & voiceFlag) ? " PMON" : "",
				(voiceFlags.non & voiceFlag) ? " NON" : "",
				(voiceFlags.eon & voiceFlag) ? " EON" : "",
				voiceIndex);
		if (ImGui::CollapsingHeader(label))
			showVoice(voice);
	}
}

//
// Show Voice State table exactly like Duckstation so can compare voices during BIOS startup (which seem different)
//
static void showVoiceStateTable(const SPU& spu)
{
	if (ImGui::BeginTable("VoiceState", 16, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
	{
		ImGui::TableSetupColumn("#");
		ImGui::TableSetupColumn("StartAddr");
		ImGui::TableSetupColumn("RepeatAddr");
		ImGui::TableSetupColumn("CurAddr");
		ImGui::TableSetupColumn("SampleIdx");
		ImGui::TableSetupColumn("SampleRate");
		ImGui::TableSetupColumn("Vol L");
		ImGui::TableSetupColumn("Vol R");
		ImGui::TableSetupColumn("ADSRPhase");
		ImGui::TableSetupColumn("ADSRVol");
		ImGui::TableSetupColumn("KON");
		ImGui::TableSetupColumn("KOFF");
		ImGui::TableSetupColumn("PMON");
		ImGui::TableSetupColumn("NON");
		ImGui::TableSetupColumn("EON");
		ImGui::TableSetupColumn("ENDX");
		ImGui::TableHeadersRow();

		const SPU::VoiceFlags& voiceFlags = spu.GetVoiceFlags();

		for (unsigned int voiceIndex = 0; voiceIndex < SPU::kVoiceCount; voiceIndex++)
		{
			const SPU::Voice& voice = spu.GetVoice(voiceIndex);

			const bool voiceInactive = voice.adsr.phase == SPU::ADSRPhase::None;
			if (voiceInactive)
				ImGui::BeginDisabled(true);

			ImGui::TableNextRow();

			// #TODO: Dim text colour (white to grey) if voice is not active

			ImGui::TableNextColumn();
			ImGui::Text("%u", voiceIndex);

			// StartAddr
			ImGui::TableNextColumn();
			ImGui::Text("%04X", voice.startAddressDiv8);

			// RepeatAddr
			ImGui::TableNextColumn();
			ImGui::Text("%04X", voice.repeatAddressDiv8);

			// CurAddr
			ImGui::TableNextColumn();
			ImGui::Text("%04X", voice.currentADPCMAddress >> 3);

			// SampleIdx
			ImGui::TableNextColumn();
			ImGui::Text("%u", voice.currentSampleIndex);

			// SampleRate
			ImGui::TableNextColumn();
			float sampleRateHz = 44100.0f * (float)voice.sampleRate / 0x1000; // 0x1000 == 44100 Hz
			ImGui::Text("%.2f", sampleRateHz);

			// VolLeft
			ImGui::TableNextColumn();
			if (voice.volumeLeft & 0x8000)
				ImGui::Text("Sweep");
			else
			{
				// In volume mode 0, the signed volume is stored in bits 14:0 (negative sign is used to invert the phase)
				float volLeft = (float)voice.volumeLeft / 0x4000;
				ImGui::Text("%u%%", (unsigned int)(volLeft * 100));
			}

			// VolRight
			ImGui::TableNextColumn();
			if (voice.volumeRight & 0x8000)
				ImGui::Text("Sweep");
			else
			{
				// In volume mode 0, the signed volume is stored in bits 14:0 (negative sign is used to invert the phase)
				float volRight = (float)voice.volumeRight / 0x4000;
				ImGui::Text("%u%%", (unsigned int)(volRight * 100));
			}

			// ADSRPhase
			ImGui::TableNextColumn();
			ImGui::Text("%s", kADSRPhaseNames[(int)voice.adsr.phase]);

			// ADSRVol
			ImGui::TableNextColumn();
			float adsrVol = (float)voice.currentADSREnvelopeLevel;
			if (adsrVol < 0.0f)
				adsrVol *= -1.0f; // negative volumes invert phase
			adsrVol /= (float)0x8000; 
			ImGui::Text("%u%%", (unsigned int)(adsrVol * 100));

			// KON
			ImGui::TableNextColumn();
			ImGui::Text("%u", (voiceFlags.kon & (1 << voiceIndex)) ? 1 : 0);

			// KOFF
			ImGui::TableNextColumn();
			ImGui::Text("%u", (voiceFlags.koff & (1 << voiceIndex)) ? 1 : 0);

			// PON
			ImGui::TableNextColumn();
			ImGui::Text("%u", (voiceFlags.pmon & (1 << voiceIndex)) ? 1 : 0);

			// NON
			ImGui::TableNextColumn();
			ImGui::Text("%u", (voiceFlags.non & (1 << voiceIndex)) ? 1 : 0);

			// EON
			ImGui::TableNextColumn();
			ImGui::Text("%u", (voiceFlags.eon & (1 << voiceIndex)) ? 1 : 0);

			// ENDX
			ImGui::TableNextColumn();
			ImGui::Text("%u", (voiceFlags.endx & (1 << voiceIndex)) ? 1 : 0);

			if (voiceInactive)
				ImGui::EndDisabled();
		}

		ImGui::EndTable();
	}
}

static void showReverb(SPU& spu)
{
	u16 baseAddressDiv8 = spu.GetReverbBaseAddressDiv8();
	ImGui::Text("Base address: %08X (%04X)", baseAddressDiv8 << 3, baseAddressDiv8);
	s16 vLOUT = spu.GetReverbOutputVolumeLeft();
	ImGui::Text("Reverb output volume L \"vLOUT\" %04X = %d = %u%%", vLOUT, vLOUT, (100 * vLOUT) / 0x8000);
	s16 vROUT = spu.GetReverbOutputVolumeRight();
	ImGui::Text("Reverb output volume R \"vROUT\" %04X = %d = %u%%", vROUT, vROUT, (100 * vROUT) / 0x8000);

	const SPU::ReverbRegisters& rev = spu.GetReverbRegisters();

	if (ImGui::BeginTable("RevRegTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
	{
		for (unsigned int regIndex = 0; regIndex < COUNTOF_ARRAY(rev.reg); regIndex++)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%02u", regIndex);
			ImGui::TableNextColumn();
			ImGui::Text("%s", kReverbRegisterNames[regIndex]);
			ImGui::TableNextColumn();
			ImGui::Text("%04X",rev.reg[regIndex]);
		}
		ImGui::EndTable();
	}

	bool debugDisableReverb = spu.DebugIsReverbDisabled();
	if (ImGui::Checkbox("Debug disable reverb", &debugDisableReverb))
		spu.DebugDisableReverb(debugDisableReverb);
}

void SPUWindow::Update(SPU& spu)
{
	if (!s_visible)
		return;

	if (ImGui::Begin("SPU", &s_visible))
	{
		if (ImGui::CollapsingHeader("SPUCNT", ImGuiTreeNodeFlags_DefaultOpen))
			showSPUCNT(spu);

		showVolumes(spu);

		if (ImGui::CollapsingHeader("Voice State", ImGuiTreeNodeFlags_DefaultOpen))
			showVoiceStateTable(spu);

		if (ImGui::CollapsingHeader("Voices"/*, ImGuiTreeNodeFlags_DefaultOpen*/))
			showVoices(spu);

		if (ImGui::CollapsingHeader("Reverb", ImGuiTreeNodeFlags_DefaultOpen))
			showReverb(spu);
		
		ImGui::End();
	}
}
