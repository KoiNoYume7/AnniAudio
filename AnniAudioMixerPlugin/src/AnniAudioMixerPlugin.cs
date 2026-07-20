namespace Loupedeck.AnniAudioMixerPlugin
{
    using System;

    // Loupedeck plugin for the AnniAudio mixer. All actions talk to the mixer
    // control API on localhost (route_cli mixer, default port 8850) through a
    // shared MixerClient; nothing touches audio devices directly.
    public class AnniAudioMixerPlugin : Plugin
    {
        private const Int32 DefaultMixerPort = 8850;

        // Gets a value indicating whether this is an API-only plugin.
        public override Boolean UsesApplicationApiOnly => true;

        // Gets a value indicating whether this is a Universal plugin or an Application plugin.
        public override Boolean HasNoApplication => true;

        public MixerClient Mixer { get; }

        public AnniAudioMixerPlugin()
        {
            PluginLog.Init(this.Log);
            PluginResources.Init(this.Assembly);

            var port = DefaultMixerPort;
            var env = Environment.GetEnvironmentVariable("ANNIAUDIO_MIXER_PORT");
            if (!String.IsNullOrEmpty(env) && Int32.TryParse(env, out var parsed))
            {
                port = parsed;
            }
            this.Mixer = new MixerClient(port);
        }

        public override void Load()
        {
            this.Mixer.ConnectionChanged += this.OnMixerConnectionChanged;
            this.Mixer.Start();
            this.OnMixerConnectionChanged(this, EventArgs.Empty);
        }

        public override void Unload()
        {
            this.Mixer.ConnectionChanged -= this.OnMixerConnectionChanged;
            this.Mixer.Dispose();
        }

        private void OnMixerConnectionChanged(Object sender, EventArgs e)
        {
            // Fully qualified: the Plugin.PluginStatus property shadows the enum type here.
            if (this.Mixer.Connected)
            {
                this.OnPluginStatusChanged(Loupedeck.PluginStatus.Normal, null);
            }
            else
            {
                this.OnPluginStatusChanged(Loupedeck.PluginStatus.Error,
                    "AnniAudio mixer is not running. Start it with start-mixer.bat (control API port 8850).");
            }
        }
    }
}
