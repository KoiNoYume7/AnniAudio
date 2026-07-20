namespace Loupedeck.AnniAudioMixerPlugin
{
    using System;
    using System.Collections.Generic;
    using System.Linq;

    // Dial adjustment for virtual cable (group) volume. One action parameter
    // per group, registered dynamically from the live mixer state:
    //   turn  -> volume up/down (2% per tick, 0-200%)
    //   press -> mute/unmute the group
    public class GroupVolumeAdjustment : PluginDynamicAdjustment
    {
        private const Int32 StepPercent = 2;

        // parameter (group id as string) -> display name it was registered with
        private readonly Dictionary<String, String> _registered = new Dictionary<String, String>();

        private AnniAudioMixerPlugin MixerPlugin => (AnniAudioMixerPlugin)this.Plugin;

        public GroupVolumeAdjustment()
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
            this.AdjustmentValueChanged(); // refresh every dial display
        }

        // Mirror the mixer's group list into this action's parameters:
        // add new groups, drop deleted ones, re-register renamed ones.
        private void SyncParameters()
        {
            var groups = this.MixerPlugin.Mixer.Groups;
            var live = groups.ToDictionary(g => g.Id.ToString(), g => g.Name);

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
                    this.AddParameter(kv.Key, $"{kv.Value} Volume", "Virtual Cables");
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
            this.MixerPlugin.Mixer.NudgeGroupVolume(actionParameter, diff * StepPercent);
            this.AdjustmentValueChanged(actionParameter);
        }

        // Dial press (the adjustment's reset command): mute/unmute.
        protected override void RunCommand(String actionParameter)
        {
            if (String.IsNullOrEmpty(actionParameter))
            {
                return;
            }
            this.MixerPlugin.Mixer.ToggleGroupMute(actionParameter);
            this.AdjustmentValueChanged(actionParameter);
        }

        protected override String GetAdjustmentValue(String actionParameter)
        {
            var g = this.MixerPlugin.Mixer.FindGroup(actionParameter);
            if (g == null)
            {
                return this.MixerPlugin.Mixer.Connected ? "?" : "--";
            }
            return g.Muted ? "MUTE" : $"{Math.Round(g.Volume)}%";
        }
    }
}
