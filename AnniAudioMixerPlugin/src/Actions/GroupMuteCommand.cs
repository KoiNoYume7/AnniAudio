namespace Loupedeck.AnniAudioMixerPlugin
{
    using System;
    using System.Collections.Generic;
    using System.Linq;

    // Touch button command that mutes/unmutes a virtual cable (group) and
    // shows the current state on the button. One parameter per group.
    // Handy for one-tap toggles like the Soundboard group.
    public class GroupMuteCommand : PluginDynamicCommand
    {
        private readonly Dictionary<String, String> _registered = new Dictionary<String, String>();

        private AnniAudioMixerPlugin MixerPlugin => (AnniAudioMixerPlugin)this.Plugin;

        public GroupMuteCommand()
            : base()
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
            this.ActionImageChanged(); // refresh every button
        }

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
                    this.AddParameter(kv.Key, $"Mute {kv.Value}", "Virtual Cables");
                    this._registered[kv.Key] = kv.Value;
                }
            }
        }

        protected override void RunCommand(String actionParameter)
        {
            if (String.IsNullOrEmpty(actionParameter))
            {
                return;
            }
            this.MixerPlugin.Mixer.ToggleGroupMute(actionParameter);
            this.ActionImageChanged(actionParameter);
        }

        protected override String GetCommandDisplayName(String actionParameter, PluginImageSize imageSize)
        {
            var g = this.MixerPlugin.Mixer.FindGroup(actionParameter);
            if (g == null)
            {
                return this.MixerPlugin.Mixer.Connected ? "?" : "mixer\noffline";
            }
            return $"{g.Name}\n{(g.Muted ? "MUTED" : $"{Math.Round(g.Volume)}%")}";
        }
    }
}
