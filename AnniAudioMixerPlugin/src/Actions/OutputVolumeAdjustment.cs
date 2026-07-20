namespace Loupedeck.AnniAudioMixerPlugin
{
    using System;
    using System.Collections.Generic;
    using System.Linq;

    // Dial adjustment for output master volume. One action parameter per
    // output (render endpoint) opened in the mixer, registered dynamically:
    //   turn -> master volume up/down (2% per tick, 0-200%)
    // The mixer API has no output mute, so pressing the dial does nothing.
    public class OutputVolumeAdjustment : PluginDynamicAdjustment
    {
        private const Int32 StepPercent = 2;

        private readonly HashSet<String> _registered = new HashSet<String>();

        private AnniAudioMixerPlugin MixerPlugin => (AnniAudioMixerPlugin)this.Plugin;

        public OutputVolumeAdjustment()
            : base(hasReset: false)
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

        private void SyncParameters()
        {
            var outputs = this.MixerPlugin.Mixer.Outputs;
            var live = new HashSet<String>(outputs.Select(o => o.Name));

            foreach (var stale in this._registered.Where(p => !live.Contains(p)).ToList())
            {
                this.RemoveParameter(stale);
                this._registered.Remove(stale);
            }
            foreach (var name in live)
            {
                if (this._registered.Add(name))
                {
                    this.AddParameter(name, $"{name} Master", "Outputs");
                }
            }
        }

        protected override void ApplyAdjustment(String actionParameter, Int32 diff)
        {
            if (String.IsNullOrEmpty(actionParameter))
            {
                return;
            }
            this.MixerPlugin.Mixer.NudgeOutputVolume(actionParameter, diff * StepPercent);
            this.AdjustmentValueChanged(actionParameter);
        }

        protected override String GetAdjustmentValue(String actionParameter)
        {
            var o = this.MixerPlugin.Mixer.FindOutput(actionParameter);
            if (o == null)
            {
                return this.MixerPlugin.Mixer.Connected ? "?" : "--";
            }
            return $"{Math.Round(o.Master)}%";
        }
    }
}
