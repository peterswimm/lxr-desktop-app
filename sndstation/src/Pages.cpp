#include "Pages.hpp"

#include "LxrInstrument.hpp"

namespace lxrapp {


Pages::Pages() {
	auto nm = [](const char* prefix, const char* rest) { return std::string(prefix) + rest; };

	auto aeg = [&](const char* p, bool repeat) {
		Page pg{"AEG", {{nm(p, "attack"), "Attack"}, {nm(p, "decay"), "Decay"}}};
		if (repeat) pg.knobs.push_back({nm(p, "repeat"), "Repeat"});
		pg.knobs.push_back({nm(p, "slope"), "Slope"});
		return pg;
	};
	auto pitchMod = [&](const char* p) {
		return Page{"MOD", {{nm(p, "pitch_decay"), "P.Decay"}, {nm(p, "pitch_slope"), "P.Slope"},
		                    {nm(p, "pitch_amount"), "P.Amount"}, {nm(p, "vel_dest"), "Vel Dest"},
		                    {nm(p, "vel_amount"), "Vel Amt"}, {nm(p, "vel_volume"), "Vel Vol"}}};
	};
	auto velMod = [&](const char* p) {
		return Page{"MOD", {{nm(p, "vel_dest"), "Vel Dest"}, {nm(p, "vel_amount"), "Vel Amt"},
		                    {nm(p, "vel_volume"), "Vel Vol"}}};
	};
	auto metal = [&](const char* p) {
		return Page{"FM", {{nm(p, "mod1_coarse"), "Mod1 Freq"}, {nm(p, "mod2_coarse"), "Mod2 Freq"},
		                   {nm(p, "mod1_gain"), "Mod1 Gain"}, {nm(p, "mod2_gain"), "Mod2 Gain"},
		                   {nm(p, "mod1_wave"), "Mod1 Wave"}, {nm(p, "mod2_wave"), "Mod2 Wave"}}};
	};
	auto trans = [&](const char* p) {
		return Page{"TRANS", {{nm(p, "trans_wave"), "Wave"}, {nm(p, "trans_vol"), "Volume"},
		                      {nm(p, "trans_freq"), "Freq"}}};
	};
	auto filter = [&](const char* p) {
		return Page{"FILTER", {{nm(p, "cutoff"), "Cutoff"}, {nm(p, "reso"), "Reso"},
		                       {nm(p, "filter_type"), "Type"}, {nm(p, "filter_drive"), "Drive"}}};
	};
	auto lfo = [&](const char* p) {
		return Page{"LFO", {{nm(p, "lfo_rate"), "Rate"}, {nm(p, "lfo_sync"), "Sync"},
		                    {nm(p, "lfo_amount"), "Amount"}, {nm(p, "lfo_wave"), "Wave"},
		                    {nm(p, "lfo_retrig"), "Retrig"}, {nm(p, "lfo_phase"), "Phase"},
		                    {nm(p, "lfo_dest"), "Dest"}}};
	};
	auto mix = [&](const char* p) {
		return Page{"MIX", {{nm(p, "volume"), "Volume"}, {nm(p, "pan"), "Pan"},
		                    {nm(p, "srate"), "S.Rate"}, {nm(p, "drive"), "Drive"},
		                    {nm(p, "out"), "Out"}}};
	};
	auto osc = [&](const char* p) {
		return Page{"OSC", {{nm(p, "coarse"), "Coarse"}, {nm(p, "fine"), "Fine"},
		                    {nm(p, "wave"), "Wave"}, {nm(p, "pwm"), "PWM"}}};
	};

	const char* drums[3][3] = {{"D1", "Drum 1", "d1_"}, {"D2", "Drum 2", "d2_"}, {"D3", "Drum 3", "d3_"}};
	for (int k = 0; k < 3; k++) {
		const char* p = drums[k][2];
		voices_.push_back({drums[k][0], drums[k][1], k,
		                   {osc(p), aeg(p, false), pitchMod(p),
		                    Page{"FM", {{nm(p, "fm_amount"), "Amount"}, {nm(p, "fm_coarse"), "Freq"},
		                                {nm(p, "fm_wave"), "Wave"}, {nm(p, "fm_mode"), "Mode"}}},
		                    trans(p), filter(p), lfo(p), mix(p)}});
	}
	voices_.push_back({"SN", "Snare", 3,
	                   {Page{"OSC", {{"sn_coarse", "Coarse"}, {"sn_fine", "Fine"}, {"sn_noise_freq", "Noise"},
	                                 {"sn_mix", "Mix"}, {"sn_wave", "Wave"}, {"sn_pwm", "PWM"}}},
	                    aeg("sn_", true), pitchMod("sn_"), trans("sn_"), filter("sn_"), lfo("sn_"), mix("sn_")}});
	voices_.push_back({"CY", "Cymbal", 4,
	                   {osc("cy_"), aeg("cy_", true), velMod("cy_"), metal("cy_"), trans("cy_"), filter("cy_"),
	                    lfo("cy_"), mix("cy_")}});
	voices_.push_back({"HH", "Hihat", 5,
	                   {osc("hh_"),
	                    Page{"AEG", {{"hh_attack", "Attack"}, {"hh_decay_closed", "Closed"},
	                                 {"hh_decay_open", "Open"}, {"hh_slope", "Slope"}}},
	                    velMod("hh_"), metal("hh_"), trans("hh_"), filter("hh_"), lfo("hh_"), mix("hh_")}});

	// FX: the type's own page, then the kit-wide sample rate (PERF_PAGE's "decimation all")
	auto fx = [](std::vector<Knob> own) {
		std::vector<Knob> k = {{"fx_type", "Type"}, {"fx_out", "Out"}, {"fx_mix", "Dry/Wet"}};
		k.insert(k.end(), own.begin(), own.end());
		return Page{"FX", k};
	};
	fx_ = {fx({}),
	       fx({{"fx_drive_type", "Shape"}, {"fx_p1", "Drive"}, {"fx_p2", "Color"}, {"fx_p3", "Tone"},
	           {"fx_p4", "Volume"}}),
	       fx({{"fx_ring_wave", "Wave"}, {"fx_p1", "Freq"}}),
	       fx({{"fx_comp_ratio", "Ratio"}, {"fx_p1", "Attack"}, {"fx_p2", "Release"}, {"fx_p3", "Thresh"},
	           {"fx_p4", "Gain"}}),
	       fx({{"fx_delay_type", "Mode"}, {"fx_p1", "Time"}, {"fx_p2", "Range"}, {"fx_p3", "Tone"},
	           {"fx_p4", "Feedback"}})};
	voices_.push_back({"FX", "FX bus", -1, {fx_[0], Page{"KIT", {{"all_srate", "S.Rate All"}}}}});
}

bool Pages::resolve(const lxr::LxrInstrument& in, std::string* missing) {
	auto find = [&](Knob& k) {
		for (const lxr::ParamDef& d : in.params())
			if (d.name == k.name) {
				k.id = d.id;
				return true;
			}
		if (missing) *missing += (missing->empty() ? "" : " ") + k.name;
		return false;
	};
	bool ok = true;
	for (Voice& v : voices_)
		for (Page& p : v.pages)
			for (Knob& k : p.knobs) ok = find(k) && ok;
	for (Page& p : fx_)
		for (Knob& k : p.knobs) ok = find(k) && ok;
	return ok;
}

} // namespace lxrapp
