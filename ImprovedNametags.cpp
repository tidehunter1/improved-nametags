#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <GWCA/Constants/Constants.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/NPC.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/ChatMgr.h>
#include <GWCA/Managers/UIMgr.h>
#include <GWCA/Utilities/Hooker.h>
#include <GWCA/Utilities/Scanner.h>
#include <GWCA/Managers/GameThreadMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/StoCMgr.h>
#include <GWCA/Packets/StoC.h>

#include <ToolboxPlugin.h>
#include <PluginUtils.h>
#include <imgui.h>

#include <vector>
#include <string>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <cwchar>
#include <optional>
#include <algorithm>
#include <array>

template<typename CacheMap>
inline void PruneCache(CacheMap& cache, uint64_t& tick, uint64_t& last_prune, uint64_t interval) {
	++tick;
	if (tick - last_prune < interval) return;
	last_prune = tick;

	for (auto it = cache.begin(); it != cache.end(); ) {
		if (tick - it->second.last_seen_tick >= interval) {
			it = cache.erase(it);
		} else {
			++it;
		}
	}
}

[[nodiscard]] inline std::vector<std::wstring> SplitWords(const std::wstring& text) {
	std::vector<std::wstring> out;
	size_t start = 0;
	while (start <= text.size()) {
		size_t pos = text.find(L' ', start);
		if (pos == std::wstring::npos) pos = text.size();
		if (pos > start) out.emplace_back(text.substr(start, pos - start));
		start = pos + 1;
	}
	return out;
}

[[nodiscard]] inline GW::Constants::ProfessionByte GetAgentProfession(const GW::AgentLiving* living) noexcept {
	if (living->IsPlayer() || living->primary != GW::Constants::ProfessionByte::None) return living->primary;
	const GW::NPC* npc = GW::Agents::GetNPCByID(living->player_number);
	return npc ? static_cast<GW::Constants::ProfessionByte>(npc->primary) : GW::Constants::ProfessionByte::None;
}

template<typename FuncPtr>
inline bool EnsureScanned(FuncPtr& cached, bool& failed, const char* pattern, const char* mask) {
	if (cached) return true;
	if (failed) return false;
	const uintptr_t addr = GW::Scanner::Find(pattern, mask);
	if (addr) cached = reinterpret_cast<FuncPtr>(addr);
	if (!cached) failed = true;
	return cached != nullptr;
}

class AgentNameCache {
public:
	struct NameLookup {
		const std::wstring* lower;
		const std::vector<std::wstring>* words;
		GW::Constants::ProfessionByte profession;
		bool resolved;
	};

	NameLookup Get(const GW::AgentLiving* living) {
		Entry& entry = cache_[living->agent_id];
		entry.last_seen_tick = tick_;
		const wchar_t* enc_name = GW::Agents::GetAgentEncName(living->agent_id);
		if (enc_name && wcsncmp(entry.last_enc, enc_name, kMaxEncLen - 1) != 0) {
			wcsncpy_s(entry.last_enc, enc_name, kMaxEncLen - 1);
			entry.buffer[0] = L'\0';
			entry.converted = false;
			entry.profession_resolved = false;
			GW::UI::AsyncDecodeStr(enc_name, entry.buffer, kBufferLen);
		}
		if (!entry.converted && entry.buffer[0] != L'\0') {
			entry.decoded_lower = entry.buffer;
			std::transform(entry.decoded_lower.begin(), entry.decoded_lower.end(), entry.decoded_lower.begin(), ::towlower);
			entry.decoded_words_lower = SplitWords(entry.decoded_lower);
			entry.converted = true;
		}
		if (!entry.profession_resolved) {
			entry.profession = GetAgentProfession(living);
			entry.profession_resolved = true;
		}
		return { &entry.decoded_lower, &entry.decoded_words_lower, entry.profession, entry.converted };
	}

	void MaybePrune() { PruneCache(cache_, tick_, last_prune_tick_, kPruneIntervalTicks); }

	void Erase(uint32_t agent_id) { cache_.erase(agent_id); }

private:
	static constexpr size_t kBufferLen = 256;
	static constexpr size_t kMaxEncLen = 64;
	static constexpr uint64_t kPruneIntervalTicks = 1800;
	struct Entry {
		wchar_t last_enc[kMaxEncLen] = {};
		wchar_t buffer[kBufferLen] = {};
		bool converted = false;
		uint64_t last_seen_tick = 0;
		std::wstring decoded_lower;
		std::vector<std::wstring> decoded_words_lower;
		GW::Constants::ProfessionByte profession = GW::Constants::ProfessionByte::None;
		bool profession_resolved = false;
	};
	std::unordered_map<uint32_t, Entry> cache_;
	uint64_t tick_ = 0, last_prune_tick_ = 0;
};

[[nodiscard]] inline std::vector<std::wstring> ParseNameList(const std::string& raw) {
	std::vector<std::wstring> out;
	std::istringstream stream(raw);
	std::string token;
	while (std::getline(stream, token, '\n')) {
		const size_t start = token.find_first_not_of(" \t\r\n");
		const size_t end = token.find_last_not_of(" \t\r\n");
		if (start == std::string::npos || end == std::string::npos) continue;

		std::wstring w = PluginUtils::StringToWString(token.substr(start, end - start + 1));
		std::transform(w.begin(), w.end(), w.begin(), ::towlower);
		if (!w.empty()) out.push_back(std::move(w));
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

struct PriorityConfig {
	std::string raw;
	uint32_t color;
};

struct ProfessionColorConfig {
	bool enabled = true;
	uint32_t color = IM_COL32(221, 221, 221, 255);
};

struct NametagSettings {
	bool recolor_quest_nametags = true, recolor_professions = false;
	bool recolor_enemy_nametags_by_profession = false;
	uint32_t quest_color = IM_COL32(255, 179, 71, 255);

	bool color_by_boss = false;
	uint32_t boss_color = IM_COL32(255, 215, 0, 255);

	std::array<ProfessionColorConfig, 11> profession_colors = {{
		{false, IM_COL32(221, 221, 221, 255)},
		{true, IM_COL32(255, 255, 136, 255)},
		{true, IM_COL32(204, 255, 153, 255)},
		{true, IM_COL32(170, 204, 255, 255)},
		{true, IM_COL32(153, 255, 204, 255)},
		{true, IM_COL32(221, 170, 255, 255)},
		{true, IM_COL32(255, 187, 187, 255)},
		{true, IM_COL32(255, 204, 238, 255)},
		{true, IM_COL32(187, 255, 255, 255)},
		{true, IM_COL32(255, 204, 153, 255)},
		{true, IM_COL32(221, 221, 255, 255)}
	}};

	bool priority_enabled = false;
	bool color_filtered = true;
	bool hide_all_other = false;
	PriorityConfig priority = {"", IM_COL32(135, 206, 250, 255)};

	bool escape_to_embark = false;
	int escape_to_embark_threshold_pct = 10;

	bool show_healthbar_all_agents = false;
};

class ImprovedNametagsPlugin : public ToolboxPlugin {
public:
	static inline ImprovedNametagsPlugin* g_plugin = nullptr;

	ImprovedNametagsPlugin() {
		g_plugin = this;
		GW::StoC::RegisterPacketCallback<GW::Packet::StoC::AgentUpdateAllegiance>(&allegiance_hook_entry_, OnAgentAllegianceChanged, 1);
		GW::StoC::RegisterPacketCallback<GW::Packet::StoC::AgentAdd>(&agent_add_hook_entry_, OnAgentAdd, 1);
		GW::StoC::RegisterPacketCallback<GW::Packet::StoC::AgentRemove>(&agent_remove_hook_entry_, OnAgentRemove, 1);
		GW::StoC::RegisterPacketCallback<GW::Packet::StoC::GenericValue>(&marker_hook_entry_, OnAgentMarkerChanged, 1);
		GW::StoC::RegisterPacketCallback<GW::Packet::StoC::MapLoaded>(&map_loaded_hook_entry_, OnMapLoaded, 1);
		GW::UI::RegisterUIMessageCallback(&chat_suppress_hook_entry_, GW::UI::UIMessage::kWriteToChatLog, OnChatLogWrite);
		GW::UI::RegisterUIMessageCallback(&chat_suppress_hook_entry_, GW::UI::UIMessage::kWriteToChatLogWithSender, OnChatLogWriteWithSender);
		GW::UI::RegisterUIMessageCallback(&preference_hook_entry_, GW::UI::UIMessage::kPreferenceFlagChanged, OnPreferenceFlagChanged);
		GW::UI::RegisterKeydownCallback(&reveal_hotkey_hook_entry_, OnRevealHotkeyDown);
		GW::UI::RegisterKeyupCallback(&reveal_hotkey_hook_entry_, OnRevealHotkeyUp);
	}

	const char* Name() const override { return "ImprovedNametags"; }

	bool* GetVisiblePtr() override { return &visible_; }

	[[nodiscard]] bool HasSettings() const override { return true; }
	void DrawSettings() override;

	template<typename Fn>
	void ForEachFlatSetting(Fn&& fn) {
		fn("recolor_quest_nametags", settings_.recolor_quest_nametags);
		fn("recolor_professions", settings_.recolor_professions);
		fn("recolor_enemy_nametags_by_profession", settings_.recolor_enemy_nametags_by_profession);
		fn("quest_color", settings_.quest_color);
		fn("color_by_boss", settings_.color_by_boss);
		fn("boss_color", settings_.boss_color);
		fn("escape_to_embark", settings_.escape_to_embark);
		fn("escape_to_embark_threshold_pct", settings_.escape_to_embark_threshold_pct);
		fn("show_healthbar_all_agents", settings_.show_healthbar_all_agents);
		fn("visible", visible_);
		fn("priority_enabled", settings_.priority_enabled);
		fn("color_filtered", settings_.color_filtered);
		fn("hide_all_other", settings_.hide_all_other);
		fn("priority_raw", settings_.priority.raw);
		fn("priority_color", settings_.priority.color);
	}

	void LoadSettings(const wchar_t* folder) override {
		ToolboxPlugin::LoadSettings(folder);
		ForEachFlatSetting([this](const char* name, auto& value) { LoadSetting(name, value); });
		for (size_t i = 1; i < settings_.profession_colors.size(); ++i) {
			const std::string prefix = "profession" + std::to_string(i);
			LoadSetting((prefix + "_enabled").c_str(), settings_.profession_colors[i].enabled);
			LoadSetting((prefix + "_color").c_str(), settings_.profession_colors[i].color);
		}
		RefreshPriorityBuffersAndLists();
	}

	void SaveSettings(const wchar_t* folder) override {
		ForEachFlatSetting([this](const char* name, auto& value) { SaveSetting(name, value); });
		for (size_t i = 1; i < settings_.profession_colors.size(); ++i) {
			const std::string prefix = "profession" + std::to_string(i);
			SaveSetting((prefix + "_enabled").c_str(), settings_.profession_colors[i].enabled);
			SaveSetting((prefix + "_color").c_str(), settings_.profession_colors[i].color);
		}
		ToolboxPlugin::SaveSettings(folder);
	}

	bool CanTerminate() override { return true; }

	void Terminate() override {
		RemoveAllegianceColorHook();
		GW::UI::RemoveUIMessageCallback(&chat_suppress_hook_entry_);
		GW::UI::RemoveUIMessageCallback(&preference_hook_entry_);
		GW::StoC::RemoveCallback<GW::Packet::StoC::AgentUpdateAllegiance>(&allegiance_hook_entry_);
		GW::StoC::RemoveCallback<GW::Packet::StoC::AgentAdd>(&agent_add_hook_entry_);
		GW::StoC::RemoveCallback<GW::Packet::StoC::AgentRemove>(&agent_remove_hook_entry_);
		GW::StoC::RemoveCallback<GW::Packet::StoC::GenericValue>(&marker_hook_entry_);
		GW::StoC::RemoveCallback<GW::Packet::StoC::MapLoaded>(&map_loaded_hook_entry_);
		GW::UI::RemoveKeydownCallback(&reveal_hotkey_hook_entry_);
		GW::UI::RemoveKeyupCallback(&reveal_hotkey_hook_entry_);
	}

	void Draw(IDirect3DDevice9*) override {
		++frame_counter_;
		EnsureAllegianceColorHookInstalled();

		if (!chat_suppress_hook_detached_ && frame_counter_ >= kStartupSuppressionFrames) {
			chat_suppress_hook_detached_ = true;
			GW::UI::RemoveUIMessageCallback(&chat_suppress_hook_entry_);
		}

		if (dirty_rescan_) {
			dirty_rescan_ = false;
			RescanAllAgentsForHealthbar();
		}

		UpdateEscapeToEmbark();

		name_cache_.MaybePrune();
		ProcessBossGlowRetries();
		ProcessPendingAllegianceRefreshes();
		ProcessPendingHideRefreshes();
	}

private:
	NametagSettings settings_;
	bool visible_ = true;
	bool dirty_rescan_ = true;
	bool ctrl_reveal_down_ = false;
	bool alt_reveal_down_ = false;
	bool hide_hotkey_active_ = false;
	bool hotkey_saved_show_healthbar_all_ = false;
	GW::HookEntry allegiance_hook_entry_;
	GW::HookEntry agent_add_hook_entry_;
	GW::HookEntry agent_remove_hook_entry_;
	GW::HookEntry marker_hook_entry_;
	GW::HookEntry map_loaded_hook_entry_;
	GW::HookEntry chat_suppress_hook_entry_;
	GW::HookEntry preference_hook_entry_;
	GW::HookEntry reveal_hotkey_hook_entry_;

	AgentNameCache name_cache_;

	uint64_t frame_counter_ = 0;
	struct BossGlowRetry {
		uint32_t agent_id;
		uint64_t scheduled_frame;
	};
	std::vector<BossGlowRetry> boss_glow_retries_;
	std::unordered_set<uint32_t> boss_glow_pending_ids_;

	void ScheduleBossGlowRetry(uint32_t agent_id) {
		if (!boss_glow_pending_ids_.insert(agent_id).second) return;
		boss_glow_retries_.push_back({agent_id, frame_counter_});
	}

	void ProcessBossGlowRetries() {
		size_t write = 0;
		for (size_t read = 0; read < boss_glow_retries_.size(); ++read) {
			const BossGlowRetry entry = boss_glow_retries_[read];
			if (frame_counter_ <= entry.scheduled_frame) {
				boss_glow_retries_[write++] = entry;
				continue;
			}
			boss_glow_pending_ids_.erase(entry.agent_id);

			GW::Agent* agent = GW::Agents::GetAgentByID(entry.agent_id);
			GW::AgentLiving* living = agent ? agent->GetAsAgentLiving() : nullptr;
			if (living && living->GetHasBossGlow()) {
				TouchAgent(entry.agent_id, true, true);
			}
		}
		boss_glow_retries_.resize(write);
	}

	bool embark_escape_armed_ = true;
	static constexpr float kEmbarkRearmHysteresisPct = 5.f;

	struct AgentState {
		bool we_applied_flag = false;
		bool has_quest_marker = false;
		bool tag_hidden = false;
	};
	std::vector<AgentState> agent_state_;

	AgentState& GetOrCreateAgentState(uint32_t agent_id) {
		if (agent_id >= agent_state_.size()) {
			agent_state_.resize(agent_id + 128);
		}
		return agent_state_[agent_id];
	}

	struct PriorityState {
		static constexpr size_t kBufSize = 1024 * 16;
		char buf[kBufSize] = {};
		std::vector<std::wstring> names;
		uint64_t pending_parse_at_ms = 0;
	};
	PriorityState priority_state_;
	static constexpr uint64_t kPriorityParseDelayMs = 150;

	void RefreshPriorityBuffersAndLists() {
		strncpy_s(priority_state_.buf, PriorityState::kBufSize, settings_.priority.raw.c_str(), _TRUNCATE);
		priority_state_.names = ParseNameList(settings_.priority.raw);
		priority_state_.pending_parse_at_ms = 0;
	}

	[[nodiscard]] bool IsPriorityMatch(const std::vector<std::wstring>& words) const noexcept {
		for (const auto& word : words) {
			if (std::binary_search(priority_state_.names.begin(), priority_state_.names.end(), word)) {
				return true;
			}
		}
		return false;
	}

	struct HideFrameCache {
		uint64_t frame = UINT64_MAX;
		bool pref_ally = false;
		bool pref_foe = false;
		bool in_outpost = false;
	};
	HideFrameCache hide_frame_cache_;

	const HideFrameCache& GetHideFrameCache() {
		if (hide_frame_cache_.frame != frame_counter_) {
			hide_frame_cache_.frame = frame_counter_;
			hide_frame_cache_.pref_ally = GW::UI::GetPreference(GW::UI::FlagPreference::AlwaysShowAllyNames);
			hide_frame_cache_.pref_foe = GW::UI::GetPreference(GW::UI::FlagPreference::AlwaysShowFoeNames);
			hide_frame_cache_.in_outpost = GW::Map::GetInstanceType() == GW::Constants::InstanceType::Outpost;
		}
		return hide_frame_cache_;
	}

	[[nodiscard]] bool ShouldApplyHideFilter(const GW::AgentLiving* living, const AgentNameCache::NameLookup& lookup) {
		if (living->GetIsDead()) return false;
		GW::AgentLiving* me = GW::Agents::GetControlledCharacter();
		if (me && living->agent_id == me->agent_id) return false;
		if (living->agent_id == GW::Agents::GetTargetId()) return false;
		if ((static_cast<uint32_t>(living->name_properties) & GW::NameTagFlags_Highlighted) != 0) return false;
		if (priority_state_.names.empty()) return false;
		if (!settings_.priority_enabled || !settings_.hide_all_other) return false;

		const HideFrameCache& fc = GetHideFrameCache();
		bool governing_pref;
		if (living->IsPlayer()) {
			governing_pref = fc.in_outpost ? fc.pref_foe : fc.pref_ally;
		} else if (living->allegiance == GW::Constants::Allegiance::Enemy) {
			governing_pref = fc.pref_foe;
		} else {
			governing_pref = fc.pref_ally;
		}
		if (!governing_pref) return false;

		if (!lookup.resolved || !lookup.words) return false;
		return !IsPriorityMatch(*lookup.words);
	}

	[[nodiscard]] std::optional<ImU32> TryGetProfessionColor(GW::Constants::ProfessionByte prof) const noexcept {
		const size_t index = static_cast<size_t>(prof);
		if (index == 0 || index >= settings_.profession_colors.size()) return std::nullopt;
		const auto& cfg = settings_.profession_colors[index];
		return cfg.enabled ? std::optional<ImU32>(cfg.color) : std::nullopt;
	}

	static void ShowHelpMarker(const char* help) {
		ImGui::SameLine();
		ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
	}

	static void RightAlignNextItem(float item_width) {
		ImGui::SameLine();
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - item_width);
	}

	void CheckboxDirty(const char* label, bool& flag) {
		if (ImGui::Checkbox(label, &flag)) dirty_rescan_ = true;
	}

	void MarkDirtyOnEdit() {
		if (ImGui::IsItemDeactivatedAfterEdit()) dirty_rescan_ = true;
	}

	void DrawCheckboxWithColorRightAligned(const char* label, bool& toggle, uint32_t& color, const char* color_id, const char* help = nullptr) {
		bool changed = ImGui::Checkbox(label, &toggle);
		if (help) ShowHelpMarker(help);
		RightAlignNextItem(ImGui::GetFrameHeight());
		ImGui::BeginDisabled(!toggle);
		ImVec4 color_vec = ImGui::ColorConvertU32ToFloat4(color);
		if (ImGui::ColorEdit3(color_id, &color_vec.x, ImGuiColorEditFlags_NoInputs)) {
			color = ImGui::ColorConvertFloat4ToU32(color_vec);
		}
		if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
		ImGui::EndDisabled();
		if (changed) dirty_rescan_ = true;
	}

	void DrawProfessionCell(size_t index) {
		ProfessionColorConfig& cfg = settings_.profession_colors[index];
		ImGui::PushID(static_cast<int>(index));
		CheckboxDirty("##enabled", cfg.enabled);
		ImGui::SameLine();
		ImGui::BeginDisabled(!cfg.enabled);
		ImVec4 color_vec = ImGui::ColorConvertU32ToFloat4(cfg.color);
		if (ImGui::ColorEdit3("##color", &color_vec.x, ImGuiColorEditFlags_NoInputs)) {
			cfg.color = ImGui::ColorConvertFloat4ToU32(color_vec);
		}
		MarkDirtyOnEdit();
		ImGui::SameLine();
		ImGui::TextUnformatted(GW::Constants::GetProfessionAcronym(static_cast<GW::Constants::Profession>(index)));
		ImGui::EndDisabled();
		ImGui::PopID();
	}

	void UpdateEscapeToEmbark() {
		if (!settings_.escape_to_embark) {
			embark_escape_armed_ = true;
			return;
		}

		GW::AgentLiving* me = GW::Agents::GetControlledCharacter();
		if (!me || me->GetIsDead()) return;
		if (!GW::Map::GetIsMapLoaded()) return;
		if (GW::Map::GetInstanceType() != GW::Constants::InstanceType::Explorable) return;
		if (GW::Map::GetMapID() == GW::Constants::MapID::Embark_Beach) return;

		const float hp_pct = std::clamp(me->hp, 0.f, 1.f) * 100.f;
		const float threshold = static_cast<float>(settings_.escape_to_embark_threshold_pct);

		if (embark_escape_armed_ && hp_pct <= threshold) {
			embark_escape_armed_ = false;
			GW::GameThread::Enqueue([] {
				GW::Map::Travel(GW::Constants::MapID::Embark_Beach);
			});
		}
		else if (!embark_escape_armed_ && hp_pct > threshold + kEmbarkRearmHysteresisPct) {
			embark_escape_armed_ = true;
		}
	}

	using AllegianceColorFn_pt = uint32_t*(__thiscall*)(void*, uint32_t*, int32_t);
	static inline AllegianceColorFn_pt AllegianceColor_Func = nullptr;
	static inline AllegianceColorFn_pt AllegianceColor_Ret = nullptr;

	static uint32_t* __thiscall OnAllegianceColor(void* ctx, uint32_t* out_color, int32_t flag) {
		GW::Hook::EnterHook();
		uint32_t* result = AllegianceColor_Ret(ctx, out_color, flag);
		if (result) {
			auto* agent = static_cast<GW::Agent*>(ctx);
			GW::AgentLiving* living = agent->GetAsAgentLiving();
			if (living) {
				g_plugin->EvaluateAgent(living, result);
			}
		}
		GW::Hook::LeaveHook();
		return result;
	}

	bool allegiance_hook_scan_failed_ = false;

	void EnsureAllegianceColorHookInstalled() {
		if (AllegianceColor_Func || allegiance_hook_scan_failed_) return;
		if (!EnsureScanned(AllegianceColor_Func, allegiance_hook_scan_failed_,
			"\x55\x8b\xec\x51\x56\x57\x8b\xf9\xf6\x87\x5c\x01\x00\x00\x08\x74\x09\xc7\x45\xfc\xa0\xa0\xa0\xff\xeb\x25\x8a\x87\xb5\x01\x00\x00\x3c\x03\x75\x09\xc7\x45\xfc\x00\x00\xff\xff\xeb\x12\xc7\x45\xfc\x00\xff\xa0\xff\x3c\x06\x74\x07",
			"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx")) {
			return;
		}
		GW::Hook::CreateHook(&AllegianceColor_Func, OnAllegianceColor, &AllegianceColor_Ret);
		GW::Hook::EnableHooks(AllegianceColor_Func);
	}

	void RemoveAllegianceColorHook() {
		if (AllegianceColor_Func) {
			GW::Hook::DisableHooks(AllegianceColor_Func);
			GW::Hook::RemoveHook(AllegianceColor_Func);
			AllegianceColor_Func = nullptr;
			AllegianceColor_Ret = nullptr;
		}
	}

	using SetNameTagBit_pt = void(__thiscall*)(void*, uint32_t, int);
	static inline SetNameTagBit_pt SetNameTagBit_Func = nullptr;

	static bool EnsureSetNameTagBitScanned() {
		static bool scan_failed = false;
		return EnsureScanned(SetNameTagBit_Func, scan_failed,
			"\x55\x8b\xec\x83\xec\x64\x83\x7d\x0c\x00\x53\x57\x8b\xf9\x8b\x57",
			"xxxxxxxxxxxxxxxx");
	}

	using QueueEventAllocator_pt = void*(__thiscall*)(void*, uint32_t);
	static inline QueueEventAllocator_pt QueueEventAllocator_Func = nullptr;

	static bool EnsureQueueEventAllocatorScanned() {
		static bool scan_failed = false;
		return EnsureScanned(QueueEventAllocator_Func, scan_failed,
			"\x55\x8b\xec\x53\x56\x57\x8b\xf9\xe8\x23\x3b\xff\xff\x8b\x55\x08\x8b\xd8\x89\x13\x8b\x57\x2c\x89\x53\x04\xc7\x43\x08\x00\x00\x00\x00\x81\xbf\x40\x01\x00\x00\xdd\xdd\xdd\xdd\x75\x14\x68\x87\x01\x00\x00\xba\x38\xdf\x93\x00\xb9\xbc\xdf\x93\x00\xe8\xcf\x23\xc9\xff\x8b\xb7\x40\x01\x00\x00\x03\xf3\x8b\x16\x8b\x4e\x04\x8b\x06\x83\xe1\xfe\x8b\x40\x04\x83\xe0\xfe\x2b\xc8\x89\x14\x31\x8b\x4e\x04\x8b\x06\x89\x48\x04\x8b\x87\x44\x01\x00\x00\x89\x06\x8b\x06\x8b\x40\x04\x89\x46\x04\x8b\x87\x44\x01\x00\x00\x89\x58\x04\x89\xb7\x44\x01\x00\x00\xa1\x38\xa8\x08\x01\x85\xc0\x0f\x84\xc0\x00\x00\x00\x50",
			"xxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????x????x????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxx");
	}

	static void TriggerAllegianceRecolor(GW::Agent* agent, uint32_t allegiance_value) {
		if (!QueueEventAllocator_Func) return;
		void* node = QueueEventAllocator_Func(agent, 8);
		if (!node) return;
		*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(node) + 0x1c) = allegiance_value;
	}

	static void RecolorAndRefreshNameTag(GW::Agent* agent, GW::AgentLiving* living) {
		TriggerAllegianceRecolor(agent, static_cast<uint32_t>(living->allegiance));

		const uint32_t current_properties = static_cast<uint32_t>(agent->name_properties);
		agent->name_properties = static_cast<GW::NameTagFlags>(current_properties | GW::NameTagFlags_PassesTransientFilter);
		GW::Agents::RefreshAgentNameTag(agent);
		agent->name_properties = static_cast<GW::NameTagFlags>(current_properties);
		GW::Agents::RefreshAgentNameTag(agent);
	}

	static bool ApplyNameTagBit(GW::Agent* agent, bool& applied, GW::NameTagFlags flag, bool want) {
		if (want == applied) return false;
		applied = want;
		if (SetNameTagBit_Func) SetNameTagBit_Func(agent, flag, want ? 1 : 0);
		return true;
	}

	static void ApplyHealthbarFlag(GW::Agent* agent, AgentState& state, bool want_flag) {
		ApplyNameTagBit(agent, state.we_applied_flag, GW::NameTagFlags_ManualTarget, want_flag);
	}

	static void ApplyHideFlag(GW::Agent* agent, AgentState& state, bool want_hidden) {
		if (!ApplyNameTagBit(agent, state.tag_hidden, GW::NameTagFlags_Suppressed, want_hidden)) return;
		if (want_hidden) {
			ApplyNameTagBit(agent, state.we_applied_flag, GW::NameTagFlags_ManualTarget, false);
		}
	}

	[[nodiscard]] static GW::AgentLiving* ValidateLivingAgent(GW::Agent* agent, GW::AgentLiving* me) {
		if (!agent || !agent->GetIsLivingType()) return nullptr;
		GW::AgentLiving* living = agent->GetAsAgentLiving();
		if (!living || living->GetIsDead()) return nullptr;
		if (me && living->agent_id == me->agent_id) return nullptr;
		return living;
	}

	[[nodiscard]] static GW::AgentLiving* GetLivingAgentByID(uint32_t agent_id, GW::Agent*& out_agent) {
		out_agent = GW::Agents::GetAgentByID(agent_id);
		return ValidateLivingAgent(out_agent, GW::Agents::GetControlledCharacter());
	}

	void OnRevealHotkeyStateChanged() {
		const bool now_active = ctrl_reveal_down_ || alt_reveal_down_;
		if (now_active == hide_hotkey_active_) return;
		hide_hotkey_active_ = now_active;
		if (now_active) {
			hotkey_saved_show_healthbar_all_ = settings_.show_healthbar_all_agents;
			settings_.show_healthbar_all_agents = false;
		} else {
			settings_.show_healthbar_all_agents = hotkey_saved_show_healthbar_all_;
		}
		dirty_rescan_ = true;
	}

	void OnRevealHotkeyKeyEvent(uint32_t key, bool down) {
		if (key == GW::UI::ControlAction_ShowOthers) ctrl_reveal_down_ = down;
		else if (key == GW::UI::ControlAction_ShowTargets) alt_reveal_down_ = down;
		else return;
		OnRevealHotkeyStateChanged();
	}

	static void OnRevealHotkeyDown(GW::HookStatus*, uint32_t key) {
		g_plugin->OnRevealHotkeyKeyEvent(key, true);
	}

	static void OnRevealHotkeyUp(GW::HookStatus*, uint32_t key) {
		g_plugin->OnRevealHotkeyKeyEvent(key, false);
	}

	void TouchAgent(uint32_t agent_id, bool recolor, bool retarget) {
		if (retarget || recolor) {
			EnsureSetNameTagBitScanned();
			EnsureQueueEventAllocatorScanned();
		}
		GW::GameThread::Enqueue([this, agent_id, recolor, retarget] {
			GW::Agent* agent;
			GW::AgentLiving* living = GetLivingAgentByID(agent_id, agent);
			if (!living) return;

			if (retarget) {
				ApplyHealthbarFlag(agent, GetOrCreateAgentState(living->agent_id), settings_.show_healthbar_all_agents);
			}
			if (recolor) {
				RecolorAndRefreshNameTag(agent, living);
			}
		});
	}

	[[nodiscard]] bool HasQuestMarker(uint32_t agent_id) const {
		return agent_id < agent_state_.size() && agent_state_[agent_id].has_quest_marker;
	}

	void RescanAllAgentsForHealthbar() {
		EnsureSetNameTagBitScanned();
		EnsureQueueEventAllocatorScanned();
		const bool want_flag = settings_.show_healthbar_all_agents;
		const bool hide_active = settings_.priority_enabled && settings_.hide_all_other;
		GW::GameThread::Enqueue([this, want_flag, hide_active] {
			GW::AgentArray* agents = GW::Agents::GetAgentArray();
			if (!agents || !agents->valid()) return;
			GW::AgentLiving* me = GW::Agents::GetControlledCharacter();
			for (GW::Agent* agent : *agents) {
				GW::AgentLiving* living = ValidateLivingAgent(agent, me);
				if (!living) continue;

				AgentState& state = GetOrCreateAgentState(living->agent_id);
				const bool want_hidden = hide_active && ShouldApplyHideFilter(living, name_cache_.Get(living));
				ApplyHealthbarFlag(agent, state, want_flag && !want_hidden);
				ApplyHideFlag(agent, state, want_hidden);
				RecolorAndRefreshNameTag(agent, living);
			}
		});
	}

	std::unordered_set<uint32_t> pending_allegiance_refresh_ids_;
	std::unordered_set<uint32_t> pending_hide_refresh_ids_;

	template<typename Fn>
	static void DrainPendingIds(std::unordered_set<uint32_t>& ids_set, Fn&& action) {
		if (ids_set.empty()) return;
		std::vector<uint32_t> ids(ids_set.begin(), ids_set.end());
		ids_set.clear();
		GW::GameThread::Enqueue([ids, action] {
			for (uint32_t agent_id : ids) {
				GW::Agent* agent;
				GW::AgentLiving* living = GetLivingAgentByID(agent_id, agent);
				if (!living) continue;
				action(agent, living);
			}
		});
	}

	void ProcessPendingAllegianceRefreshes() {
		EnsureSetNameTagBitScanned();
		EnsureQueueEventAllocatorScanned();
		DrainPendingIds(pending_allegiance_refresh_ids_, [](GW::Agent* agent, GW::AgentLiving* living) {
			RecolorAndRefreshNameTag(agent, living);
		});
	}

	void CheckHidePriority(GW::AgentLiving* living, const AgentNameCache::NameLookup& lookup) {
		if (ShouldApplyHideFilter(living, lookup) != GetOrCreateAgentState(living->agent_id).tag_hidden) {
			pending_hide_refresh_ids_.insert(living->agent_id);
		}
	}

	void ProcessPendingHideRefreshes() {
		EnsureSetNameTagBitScanned();
		DrainPendingIds(pending_hide_refresh_ids_, [this](GW::Agent* agent, GW::AgentLiving* living) {
			ApplyHideFlag(agent, GetOrCreateAgentState(living->agent_id), ShouldApplyHideFilter(living, name_cache_.Get(living)));
		});
	}

	static void OnAgentAllegianceChanged(GW::HookStatus*, GW::Packet::StoC::AgentUpdateAllegiance* pak) {
		if (!pak) return;
		g_plugin->pending_allegiance_refresh_ids_.insert(pak->agent_id);
	}

	static void OnAgentAdd(GW::HookStatus*, GW::Packet::StoC::AgentAdd* pak) {
		if (!pak) return;
		auto* self = g_plugin;
		self->TouchAgent(pak->agent_id, true, true);
	}

	static void OnMapLoaded(GW::HookStatus*, GW::Packet::StoC::MapLoaded*) {
		auto* self = g_plugin;
		self->agent_state_.clear();
		self->boss_glow_retries_.clear();
		self->boss_glow_pending_ids_.clear();
		self->pending_allegiance_refresh_ids_.clear();
		self->pending_hide_refresh_ids_.clear();
		self->dirty_rescan_ = true;
	}

	static void OnAgentRemove(GW::HookStatus*, GW::Packet::StoC::AgentRemove* pak) {
		if (!pak) return;
		auto* self = g_plugin;
		if (pak->agent_id < self->agent_state_.size()) {
			self->agent_state_[pak->agent_id] = AgentState{};
		}
		self->name_cache_.Erase(pak->agent_id);
	}

	static void OnAgentMarkerChanged(GW::HookStatus*, GW::Packet::StoC::GenericValue* pak) {
		if (!pak) return;
		if (pak->value_id != GW::Packet::StoC::GenericValueID::apply_marker
			&& pak->value_id != GW::Packet::StoC::GenericValueID::remove_marker) return;

		auto* self = g_plugin;
		self->GetOrCreateAgentState(pak->agent_id).has_quest_marker =
			pak->value_id == GW::Packet::StoC::GenericValueID::apply_marker;
		self->TouchAgent(pak->agent_id, true, true);
	}

	static constexpr int kStartupSuppressionFrames = 300;
	bool chat_suppress_hook_detached_ = false;

	[[nodiscard]] static bool ShouldSuppressWarning(uint32_t channel, const wchar_t* message) {
		if (channel != GW::Chat::Channel::CHANNEL_GWCA2 && channel != GW::Chat::Channel::CHANNEL_WARNING) return false;
		if (!message) return false;
		return wcsstr(message, L"Plugins") != nullptr;
	}

	static void OnChatLogWrite(GW::HookStatus* status, GW::UI::UIMessage, void* wParam, void*) {
		auto* msg = static_cast<GW::UI::UIPacket::kWriteToChatLog*>(wParam);
		if (!msg) return;
		if (ShouldSuppressWarning(static_cast<uint32_t>(msg->channel), msg->message)) {
			status->blocked = true;
		}
	}

	static void OnChatLogWriteWithSender(GW::HookStatus* status, GW::UI::UIMessage, void* wParam, void*) {
		auto* msg = static_cast<GW::UI::UIPacket::kWriteToChatLogWithSender*>(wParam);
		if (!msg) return;
		if (ShouldSuppressWarning(msg->channel, msg->message)) {
			status->blocked = true;
		}
	}

	static void OnPreferenceFlagChanged(GW::HookStatus*, GW::UI::UIMessage, void* wParam, void*) {
		auto* msg = static_cast<GW::UI::UIPacket::kPreferenceFlagChanged*>(wParam);
		if (!msg) return;
		if (msg->preference_id == GW::UI::FlagPreference::AlwaysShowAllyNames
			|| msg->preference_id == GW::UI::FlagPreference::AlwaysShowFoeNames) {
			g_plugin->dirty_rescan_ = true;
		}
	}

	void EvaluateAgent(GW::AgentLiving* living, uint32_t* out_color) {
		if (living->allegiance == GW::Constants::Allegiance::Npc_Minipet) return;

		const bool is_enemy = living->allegiance == GW::Constants::Allegiance::Enemy;
		const bool need_prof = is_enemy
			? settings_.recolor_enemy_nametags_by_profession
			: settings_.recolor_professions;
		const bool need_names = settings_.priority_enabled
			&& (settings_.color_filtered || settings_.hide_all_other);

		AgentNameCache::NameLookup lookup{};
		if (need_names || need_prof) {
			lookup = name_cache_.Get(living);
		}

		if (const auto color = DecideAgentColor(living, is_enemy, need_prof, lookup)) {
			*out_color = static_cast<uint32_t>(*color);
		}

		if (settings_.priority_enabled && settings_.hide_all_other) {
			CheckHidePriority(living, lookup);
		}
	}

	[[nodiscard]] std::optional<ImU32> DecideAgentColor(const GW::AgentLiving* living, bool is_enemy, bool need_prof, const AgentNameCache::NameLookup& lookup) {
		if (settings_.priority_enabled && settings_.color_filtered
			&& lookup.words && IsPriorityMatch(*lookup.words)) {
			return settings_.priority.color;
		}

		if (is_enemy) {
			if (settings_.color_by_boss) {
				if (living->GetHasBossGlow()) {
					return settings_.boss_color;
				}
				ScheduleBossGlowRetry(living->agent_id);
			}
			if (need_prof) {
				if (const auto color = TryGetProfessionColor(lookup.profession)) {
					return color;
				}
			}
			return std::nullopt;
		}

		if (settings_.recolor_quest_nametags
			&& (living->GetHasQuest() || HasQuestMarker(living->agent_id))) {
			return settings_.quest_color;
		}

		if (need_prof
			&& living->allegiance == GW::Constants::Allegiance::Ally_NonAttackable) {
			if (const auto color = TryGetProfessionColor(lookup.profession)) {
				return color;
			}
		}

		return std::nullopt;
	}

	void DrawPriorityInput(const char* input_id, PriorityState& state, std::string& raw) {
		static constexpr int kMaxNameLength = 40;
		const float box_width = (ImGui::CalcTextSize("M").x * kMaxNameLength + ImGui::GetStyle().FramePadding.x * 2.0f) * 0.5f;
		if (ImGui::InputTextMultiline(input_id, state.buf, PriorityState::kBufSize, ImVec2(box_width, ImGui::GetTextLineHeight() * 8.f))) {
			state.pending_parse_at_ms = GetTickCount64() + kPriorityParseDelayMs;
		}
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			raw = state.buf;
			state.names = ParseNameList(raw);
			state.pending_parse_at_ms = 0;
			dirty_rescan_ = true;
		}
		else if (state.pending_parse_at_ms != 0 && GetTickCount64() >= state.pending_parse_at_ms) {
			raw = state.buf;
			state.names = ParseNameList(raw);
			state.pending_parse_at_ms = 0;
		}
	}

	void DrawSettingsInternal() {
		ImGui::SeparatorText("Nametags");

		DrawCheckboxWithColorRightAligned("Color by boss", settings_.color_by_boss, settings_.boss_color, "##color_by_boss", "Overrides other nametag coloring (except Priority) for agents with the boss glow");
		DrawCheckboxWithColorRightAligned("Color by quest", settings_.recolor_quest_nametags, settings_.quest_color, "##color_quest");

		CheckboxDirty("Priority System", settings_.priority_enabled);
		ShowHelpMarker("One name per line. Any single word (e.g. \"Monk\") matches any name containing that exact word.");

		ImGui::BeginDisabled(!settings_.priority_enabled);
		ImGui::Indent();

		CheckboxDirty("Color filtered", settings_.color_filtered);
		RightAlignNextItem(ImGui::GetFrameHeight());
		ImVec4 priority_color_vec = ImGui::ColorConvertU32ToFloat4(settings_.priority.color);
		if (ImGui::ColorEdit3("##priority_color", &priority_color_vec.x, ImGuiColorEditFlags_NoInputs)) {
			settings_.priority.color = ImGui::ColorConvertFloat4ToU32(priority_color_vec);
		}
		MarkDirtyOnEdit();

		CheckboxDirty("Hide all other", settings_.hide_all_other);
		ShowHelpMarker("Hides the nametag entirely for any agent that doesn't match the list below. Only takes effect while the game's own matching \"Always Show\" name-tag option is enabled.");

		char priority_header_label[48];
		snprintf(priority_header_label, sizeof(priority_header_label), "Priority Names (%zu)###priority_names", priority_state_.names.size());
		ImGui::BeginDisabled(!settings_.color_filtered && !settings_.hide_all_other);
		if (ImGui::TreeNodeEx(priority_header_label, ImGuiTreeNodeFlags_FramePadding)) {
			DrawPriorityInput("##priority_input", priority_state_, settings_.priority.raw);
			ImGui::TreePop();
		}
		ImGui::EndDisabled();

		ImGui::Unindent();
		ImGui::EndDisabled();

		ImGui::Spacing();
		CheckboxDirty("Color allies by profession", settings_.recolor_professions);
		CheckboxDirty("Color foes by profession", settings_.recolor_enemy_nametags_by_profession);
		ShowHelpMarker("Uses the profession colors below - if a monster's profession can't be determined, its normal color is used instead.");

		ImGui::BeginDisabled(!settings_.recolor_professions && !settings_.recolor_enemy_nametags_by_profession);
		size_t enabled_profession_count = 0;
		for (size_t i = 1; i < settings_.profession_colors.size(); ++i) {
			if (settings_.profession_colors[i].enabled) ++enabled_profession_count;
		}
		char profession_header_label[48];
		snprintf(profession_header_label, sizeof(profession_header_label), "Profession Colors (%zu)###profession_colors", enabled_profession_count);
		if (ImGui::TreeNodeEx(profession_header_label, ImGuiTreeNodeFlags_FramePadding)) {
			if (ImGui::BeginTable("##profession_colors_table", 5)) {
				for (int c = 0; c < 5; ++c) {
					ImGui::TableSetupColumn("##pcol", ImGuiTableColumnFlags_WidthStretch);
				}
				for (size_t row = 0; row < 2; ++row) {
					ImGui::TableNextRow();
					for (size_t col = 0; col < 5; ++col) {
						ImGui::TableNextColumn();
						DrawProfessionCell(row * 5 + col + 1);
					}
				}
				ImGui::EndTable();
			}
			ImGui::TreePop();
		}
		ImGui::EndDisabled();

		ImGui::Spacing();
		ImGui::SeparatorText("Safety");

		ImGui::Checkbox("Escape to Embark Beach", &settings_.escape_to_embark);
		ShowHelpMarker("Teleports you to Embark Beach based on health % threshold");

		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		ImGui::DragInt("##embark_threshold", &settings_.escape_to_embark_threshold_pct, 1.0f, 1, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);

		ImGui::Spacing();
		ImGui::SeparatorText("Health Bars");
		CheckboxDirty("Show health bar on all agents", settings_.show_healthbar_all_agents);
		ShowHelpMarker("Shows the same floating health bar you get from hovering over a unit, on all nearby agents at once.");
	}
};

void ImprovedNametagsPlugin::DrawSettings() {
	ToolboxPlugin::DrawSettings();
	DrawSettingsInternal();
}

DLLAPI ToolboxPlugin* ToolboxPluginInstance() {
	static ImprovedNametagsPlugin instance;
	return &instance;
}
