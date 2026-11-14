"""RNBO parameter set for the LXR engine.

Every LXR sound parameter becomes one RNBO @param that keeps the firmware's own
value range (mostly 0-127, or the index of a menu entry), so values can be copied
1:1 between an LXR kit and the RNBO patch. The parameter's LXR number (PAR_*) is
what the engine's applyParam() dispatches on.
"""
from lxrparams import par_enum, init_kit, mod_targets

WAVES = ["Sine", "Tri", "Saw", "Rect", "Noise", "PWM"]
FILTERS = ["LP", "HP", "BP", "UnityBP", "Notch", "Peak", "LP2", "Off"]
LFO_WAVES = ["Sine", "Tri", "SawUp", "SawDown", "Square", "Random", "ExpUp", "ExpDown"]
RETRIG = ["Off", "Drum1", "Drum2", "Drum3", "Snare", "Cymbal", "Hihat"]
SYNC = ["Off", "4/1", "2/1", "1/1", "1/2", "1/3", "1/4", "1/6", "1/8", "1/12", "1/16", "1/32"]
TRANSIENTS = ["Snap", "Offset", "Click", "Click2", "Tick", "Kick", "Rim", "Drip",
              "Hat", "Clap", "Kick2", "Snare", "Tom", "Snap2"]
OUTPUTS = ["Main", "Aux", "MainL", "MainR", "AuxL", "AuxR", "FxBus"]
FX_OUTPUTS = OUTPUTS[:6]
FX_TYPES = ["Off", "Drive", "RingMod", "Comp", "Delay"]
DIST_TYPES = ["Tube", "Fold", "Clip"]
RATIOS = ["1:1", "2:1", "3:1", "4:1", "6:1", "8:1"]
DELAY_TYPES = ["Delay", "PingPong"]
MIX_FM = ["FM", "Mix"]
ON_OFF = ["Off", "On"]

VOICES = ["d1", "d2", "d3", "sn", "cy", "hh"]
VOICE_TITLES = ["Drum1", "Drum2", "Drum3", "Snare", "Cymbal", "Hihat"]

def target_names(defs):
    """Enum labels for the LFO/velocity destination params (index = modTargets index).

    Labels are built from the target's RNBO param name, e.g. d1_cutoff -> "Drum1 Cutoff".
    The FX parameters appear once per voice in the firmware's list; they keep the voice
    in their label so every entry stays unique.
    """
    by_par = {d["par"]: d["name"] for d in defs}
    offsets = [2, 40, 78, 116, 153, 191]  # modTargetVoiceOffsets[].start
    names = []
    for i, (_text, par) in enumerate(mod_targets()):
        if i == 0:
            names.append("Off")
            continue
        name = by_par[par]
        pre, rest = name.split("_", 1)
        words = "".join(w.capitalize() for w in rest.split("_"))
        if pre in VOICES:
            label = f"{VOICE_TITLES[VOICES.index(pre)]} {words}"
        elif pre == "fx":
            v = max(k for k, o in enumerate(offsets) if i >= o)
            label = f"{VOICE_TITLES[v]} Fx{words}"
        else:
            label = f"All {words}"
        names.append(label)
    assert len(set(names)) == len(names), "duplicate target labels"
    return names


def _p(par, name, lo=0, hi=127, enum=None):
    return {"par": par, "name": name, "min": lo, "max": hi, "enum": enum}


def definitions():
    """List of params in display order: dicts with par, name, min, max, enum, default."""
    e = par_enum()
    kit = init_kit()
    tnames = ["(targets)"]  # filled in below, once every param has a name
    out = []

    def add(parname, name, **kw):
        d = _p(e[parname], name, **kw)
        if d["enum"]:
            d["min"], d["max"] = 0, len(d["enum"]) - 1
        out.append(d)

    for v, pre in enumerate(VOICES):
        n = v + 1
        drum = v < 3
        # oscillator
        wave = {0: "PAR_OSC_WAVE_DRUM1", 1: "PAR_OSC_WAVE_DRUM2", 2: "PAR_OSC_WAVE_DRUM3",
                3: "PAR_OSC_WAVE_SNARE", 4: "PAR_WAVE1_CYM", 5: "PAR_WAVE1_HH"}[v]
        add(wave, f"{pre}_wave", enum=WAVES)
        add(f"PAR_COARSE{n}", f"{pre}_coarse")
        add(f"PAR_FINE{n}", f"{pre}_fine")
        add(f"PAR_PWM_{n}", f"{pre}_pwm")
        if v == 3:
            add("PAR_NOISE_FREQ1", "sn_noise_freq")
            add("PAR_MIX1", "sn_mix")
        # amp envelope
        add(f"PAR_VELOA{n}", f"{pre}_attack")
        if v == 5:
            add("PAR_VELOD6_CLOSED", "hh_decay_closed")
            add("PAR_VELOD6_OPEN", "hh_decay_open")
        else:
            add(f"PAR_VELOD{n}", f"{pre}_decay")
        add(f"PAR_VOL_SLOPE{n}", f"{pre}_slope")
        if v in (3, 4):
            add(f"PAR_REPEAT{n}", f"{pre}_repeat")
        # pitch envelope (drums and snare)
        if v < 4:
            add(f"PAR_MOD_EG{n}", f"{pre}_pitch_decay")
            add(f"PAR_PITCH_SLOPE{n}", f"{pre}_pitch_slope")
            add(f"PAR_MODAMNT{n}", f"{pre}_pitch_amount")
        # FM / modulator oscillators
        if drum:
            add(f"PAR_FMAMNT{n}", f"{pre}_fm_amount")
            add(f"PAR_FM_FREQ{n}", f"{pre}_fm_coarse")
            add(f"PAR_MOD_WAVE_DRUM{n}", f"{pre}_fm_wave", enum=WAVES)
            add(f"PAR_MIX_MOD_{n}", f"{pre}_fm_mode", enum=MIX_FM)
        elif v == 4:
            add("PAR_MOD_OSC_F1_CYM", "cy_mod1_coarse")
            add("PAR_MOD_OSC_GAIN1_CYM", "cy_mod1_gain")
            add("PAR_WAVE2_CYM", "cy_mod1_wave", enum=WAVES)
            add("PAR_MOD_OSC_F2_CYM", "cy_mod2_coarse")
            add("PAR_MOD_OSC_GAIN2_CYM", "cy_mod2_gain")
            add("PAR_WAVE3_CYM", "cy_mod2_wave", enum=WAVES)
        elif v == 5:
            add("PAR_MOD_OSC_F1", "hh_mod1_coarse")
            add("PAR_MOD_OSC_GAIN1", "hh_mod1_gain")
            add("PAR_WAVE2_HH", "hh_mod1_wave", enum=WAVES)
            add("PAR_MOD_OSC_F2", "hh_mod2_coarse")
            add("PAR_MOD_OSC_GAIN2", "hh_mod2_gain")
            add("PAR_WAVE3_HH", "hh_mod2_wave", enum=WAVES)
        # transient
        add(f"PAR_TRANS{n}_WAVE", f"{pre}_trans_wave", enum=TRANSIENTS)
        add(f"PAR_TRANS{n}_VOL", f"{pre}_trans_vol")
        add(f"PAR_TRANS{n}_FREQ", f"{pre}_trans_freq")
        # filter
        add(f"PAR_FILTER_FREQ_{n}", f"{pre}_cutoff")
        add(f"PAR_RESO_{n}", f"{pre}_reso")
        add(f"PAR_FILTER_TYPE_{n}", f"{pre}_filter_type", enum=FILTERS)
        add(f"PAR_FILTER_DRIVE_{n}", f"{pre}_filter_drive")
        # velocity modulation
        add(f"PAR_VELO_MOD_AMT_{n}", f"{pre}_vel_amount")
        add(f"PAR_VEL_DEST_{n}", f"{pre}_vel_dest", enum=tnames)
        add(f"PAR_VOLUME_MOD_ON_OFF{n}", f"{pre}_vel_volume", enum=ON_OFF)
        # LFO
        add(f"PAR_WAVE_LFO{n}", f"{pre}_lfo_wave", enum=LFO_WAVES)
        add(f"PAR_FREQ_LFO{n}", f"{pre}_lfo_rate")
        add(f"PAR_SYNC_LFO{n}", f"{pre}_lfo_sync", enum=SYNC)
        add(f"PAR_AMOUNT_LFO{n}", f"{pre}_lfo_amount")
        add(f"PAR_TARGET_LFO{n}", f"{pre}_lfo_dest", enum=tnames)
        add(f"PAR_RETRIGGER_LFO{n}", f"{pre}_lfo_retrig", enum=RETRIG)
        add(f"PAR_OFFSET_LFO{n}", f"{pre}_lfo_phase")
        # mix
        add(f"PAR_VOL{n}", f"{pre}_volume")
        add(f"PAR_PAN{n}", f"{pre}_pan")
        drive = {0: "PAR_DRIVE1", 1: "PAR_DRIVE2", 2: "PAR_DRIVE3", 3: "PAR_SNARE_DISTORTION",
                 4: "PAR_CYMBAL_DISTORTION", 5: "PAR_HAT_DISTORTION"}[v]
        add(drive, f"{pre}_drive")
        add(f"PAR_VOICE_DECIMATION{n}", f"{pre}_srate")
        add(f"PAR_AUDIO_OUT{n}", f"{pre}_out", enum=OUTPUTS)

    add("PAR_VOICE_DECIMATION_ALL", "all_srate")
    add("PAR_FX_TYPE", "fx_type", enum=FX_TYPES)
    add("PAR_FX_DRY_WET", "fx_mix", hi=100)
    add("PAR_FX_P1", "fx_p1")
    add("PAR_FX_P2", "fx_p2")
    add("PAR_FX_P3", "fx_p3")
    add("PAR_FX_P4", "fx_p4")
    add("PAR_FX_DIST_TYPE", "fx_drive_type", enum=DIST_TYPES)
    add("PAR_FX_RATIO", "fx_comp_ratio", enum=RATIOS)
    add("PAR_FX_WAVE", "fx_ring_wave", enum=WAVES)
    add("PAR_FX_DELAY_TYPE", "fx_delay_type", enum=DELAY_TYPES)
    add("PAR_FX_ROUTING", "fx_out", enum=FX_OUTPUTS)

    tnames = target_names(out)
    for d in out:
        if d["enum"] == ["(targets)"]:
            d["enum"] = tnames
            d["max"] = len(tnames) - 1
    for d in out:
        d["default"] = min(max(kit[d["par"]], d["min"]), d["max"])
    names = [d["name"] for d in out]
    assert len(set(names)) == len(names)
    pars = [d["par"] for d in out]
    assert len(set(pars)) == len(pars)
    return out


if __name__ == "__main__":
    defs = definitions()
    for d in defs:
        print(d["par"], d["name"], d["min"], d["max"], d["default"], (d["enum"] or [])[:4])
    print(len(defs), "params")
