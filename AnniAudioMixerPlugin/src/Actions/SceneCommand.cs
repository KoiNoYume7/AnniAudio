namespace Loupedeck.AnniAudioMixerPlugin
{
    using System;
    using System.Collections.Generic;
    using System.Linq;

    // Touch button that applies a saved scene (a named level overlay:
    // volumes, mutes, send gains, output masters). One parameter per scene
    // file in config/scenes/, registered dynamically. Press "Night" and the
    // whole mixer snaps to your night levels - instantly, no device reopen.
    public class SceneCommand : PluginDynamicCommand
    {
        private readonly HashSet<String> _registered = new HashSet<String>();

        private AnniAudioMixerPlugin MixerPlugin => (AnniAudioMixerPlugin)this.Plugin;

        public SceneCommand()
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
            this.ActionImageChanged();
        }

        private void SyncParameters()
        {
            var live = new HashSet<String>(this.MixerPlugin.Mixer.Scenes);

            foreach (var stale in this._registered.Where(s => !live.Contains(s)).ToList())
            {
                this.RemoveParameter(stale);
                this._registered.Remove(stale);
            }
            foreach (var name in live)
            {
                if (this._registered.Add(name))
                {
                    this.AddParameter(name, $"Scene: {name}", "Scenes");
                }
            }
        }

        protected override void RunCommand(String actionParameter)
        {
            if (String.IsNullOrEmpty(actionParameter))
            {
                return;
            }
            this.MixerPlugin.Mixer.ApplyScene(actionParameter);
        }

        protected override String GetCommandDisplayName(String actionParameter, PluginImageSize imageSize)
            => String.IsNullOrEmpty(actionParameter) ? null : $"Scene\n{actionParameter}";
    }
}
