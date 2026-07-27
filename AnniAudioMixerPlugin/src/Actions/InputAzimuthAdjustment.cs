namespace Loupedeck.AnniAudioMixerPlugin
{
    using System;
    using System.Collections.Generic;
    using System.Linq;

    // Dial adjustment for per-input HRTF azimuth. One action parameter per spatial
    // input, registered dynamically from the live mixer state.
    //   turn  -> rotate azimuth (5 degrees per tick, -180 to +180)
    //   press -> reset azimuth to 0 (front)
    public class InputAzimuthAdjustment : PluginDynamicAdjustment
    {
        private const Int32 StepDegrees = 5;

        // parameter (input id as string) -> display name it was registered with
        private readonly Dictionary<String, String> _registered = new Dictionary<String, String>();

        private AnniAudioMixerPlugin MixerPlugin => (AnniAudioMixerPlugin)this.Plugin;

        public InputAzimuthAdjustment()
            : base(hasReset: true)
        {
        }

        protected override Boolean OnLoad()
        {
            this.MixerPlugin.Mixer.StateChanged += this.OnMixerChanged;
            this.MixerPlugin.Mixer.ConnectionChanged += this.OnMixerChanged;
            this.SyncParameters();
            return true;
        }

        protected override Boolean OnUnload()
        {
            this.MixerPlugin.Mixer.StateChanged -= this.OnMixerChanged;
            this.MixerPlugin.Mixer.ConnectionChanged -= this.OnMixerChanged;
            return true;
        }

        private void OnMixerChanged(Object sender, EventArgs e)
        {
            this.SyncParameters();
            this.AdjustmentValueChanged();
        }

        // Mirror the mixer's spatial inputs into this action's parameters.
        private void SyncParameters()
        {
            var inputs = this.MixerPlugin.Mixer.Inputs;
            var live = inputs.Where(i => i.Spatial).ToDictionary(i => i.Id.ToString(), i => i.Name);

            foreach (var stale in this._registered.Keys.Where(p =>
                         !live.TryGetValue(p, out var name) || name != this._registered[p]).ToList())
            {
                this.RemoveParameter(stale);
                this._registered.Remove(stale);
            }
            foreach (var kv in live)
            {
                if (!this._registered.ContainsKey(kv.Key))
                {
                    this.AddParameter(kv.Key, $"{kv.Value} Azimuth", "Spatial Inputs");
                    this._registered[kv.Key] = kv.Value;
                }
            }
        }

        protected override void ApplyAdjustment(String actionParameter, Int32 diff)
        {
            if (String.IsNullOrEmpty(actionParameter))
            {
                return;
            }
            this.MixerPlugin.Mixer.NudgeInputAzimuth(actionParameter, diff * StepDegrees);
            this.AdjustmentValueChanged(actionParameter);
        }

        // Dial press resets azimuth to 0 (front).
        protected override void RunCommand(String actionParameter)
        {
            if (String.IsNullOrEmpty(actionParameter))
            {
                return;
            }
            var input = this.MixerPlugin.Mixer.FindInput(actionParameter);
            if (input == null || !input.Spatial)
            {
                return;
            }
            this.MixerPlugin.Mixer.NudgeInputAzimuth(actionParameter,
                (Int32)Math.Round(-input.Azimuth));
            this.AdjustmentValueChanged(actionParameter);
        }

        protected override String GetAdjustmentValue(String actionParameter)
        {
            var input = this.MixerPlugin.Mixer.FindInput(actionParameter);
            if (input == null)
            {
                return this.MixerPlugin.Mixer.Connected ? "?" : "--";
            }
            if (!input.Spatial)
            {
                return "off";
            }
            var dir = input.Azimuth switch
            {
                0.0 => "C",
                > 0.0 and < 90.0 => $"L{Math.Round(input.Azimuth)}",
                >= 90.0 => $"L{Math.Round(input.Azimuth)}",
                < 0.0 and > -90.0 => $"R{Math.Abs(Math.Round(input.Azimuth))}",
                _ => $"R{Math.Abs(Math.Round(input.Azimuth))}",
            };
            return dir;
        }
    }
}
