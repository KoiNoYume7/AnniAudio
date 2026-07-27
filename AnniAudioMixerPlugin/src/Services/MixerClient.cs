namespace Loupedeck.AnniAudioMixerPlugin
{
    using System;
    using System.Collections.Generic;
    using System.Linq;
    using System.Net.Http;
    using System.Text;
    using System.Text.Json;
    using System.Threading;

    public sealed class MixerGroup
    {
        public UInt32 Id { get; init; }
        public String Name { get; init; }
        public Double Volume { get; set; }
        public Boolean Muted { get; set; }
    }

    public sealed class MixerOutput
    {
        public String Name { get; init; }
        public Double Master { get; set; }
        public Boolean Muted { get; set; }
    }

    public sealed class MixerInput
    {
        public UInt32 Id { get; init; }
        public String Name { get; init; }
        public Boolean Spatial { get; set; }
        public Double Azimuth { get; set; }
        public Double Elevation { get; set; }
    }

    // Client for the mixer control API served by `route_cli mixer` on localhost
    // (see docs/MIXER-CONTROL-API.md in the AnniAudio repository).
    //
    // A poll thread keeps a snapshot of groups/outputs fresh and raises
    // StateChanged when anything relevant to the device display changes.
    // Volume/mute writes are coalesced per target by a send thread, so a fast
    // dial turn produces a handful of PATCHes instead of hundreds. Values
    // written locally are held for a short period against poll overwrites,
    // which keeps the dial display from snapping back to a stale server value.
    public sealed class MixerClient : IDisposable
    {
        private const Int32 PollIntervalMs = 1000;
        private const Int32 SendIntervalMs = 50;
        private const Int32 HoldMs = 1500;

        private readonly String _baseUrl;
        private readonly HttpClient _http = new HttpClient { Timeout = TimeSpan.FromSeconds(3) };

        private readonly Object _lock = new Object();
        private List<MixerInput> _inputs = new List<MixerInput>();
        private List<MixerGroup> _groups = new List<MixerGroup>();
        private List<MixerOutput> _outputs = new List<MixerOutput>();
        private List<String> _scenes = new List<String>();
        private readonly Dictionary<String, Func<HttpRequestMessage>> _pendingSends = new Dictionary<String, Func<HttpRequestMessage>>();
        private readonly Dictionary<String, DateTime> _holdUntil = new Dictionary<String, DateTime>();

        private Thread _pollThread;
        private Thread _sendThread;
        private volatile Boolean _running;

        public Boolean Connected { get; private set; }

        public event EventHandler StateChanged;
        public event EventHandler ConnectionChanged;

        public MixerClient(Int32 port) => this._baseUrl = $"http://127.0.0.1:{port}";

        public void Start()
        {
            this._running = true;
            this._pollThread = new Thread(this.PollLoop) { IsBackground = true, Name = "AnniAudioMixerPoll" };
            this._pollThread.Start();
            this._sendThread = new Thread(this.SendLoop) { IsBackground = true, Name = "AnniAudioMixerSend" };
            this._sendThread.Start();
        }

        public void Dispose()
        {
            this._running = false;

            // Give the background workers a chance to exit before disposing the
            // HttpClient they use. A missed send or poll is harmless; touching a
            // disposed client is not.
            this._pollThread?.Join(1500);
            this._sendThread?.Join(500);

            this._http.Dispose();
        }

        public MixerInput[] Inputs
        {
            get { lock (this._lock) { return this._inputs.ToArray(); } }
        }

        public MixerGroup[] Groups
        {
            get { lock (this._lock) { return this._groups.ToArray(); } }
        }

        public MixerOutput[] Outputs
        {
            get { lock (this._lock) { return this._outputs.ToArray(); } }
        }

        public String[] Scenes
        {
            get { lock (this._lock) { return this._scenes.ToArray(); } }
        }

        public void ApplyScene(String name)
        {
            lock (this._lock)
            {
                this.QueueSendLocked($"scene{name}", "POST", "/api/scenes/apply",
                    JsonSerializer.Serialize(new { name }));
            }
        }

        public MixerInput FindInput(String idStr)
        {
            lock (this._lock)
            {
                return this._inputs.FirstOrDefault(i => i.Id.ToString() == idStr);
            }
        }

        public MixerGroup FindGroup(String idStr)
        {
            lock (this._lock)
            {
                return this._groups.FirstOrDefault(g => g.Id.ToString() == idStr);
            }
        }

        public MixerOutput FindOutput(String name)
        {
            lock (this._lock)
            {
                return this._outputs.FirstOrDefault(o => o.Name == name);
            }
        }

        public void NudgeGroupVolume(String idStr, Int32 diffPercent)
        {
            lock (this._lock)
            {
                var g = this._groups.FirstOrDefault(x => x.Id.ToString() == idStr);
                if (g == null)
                {
                    return;
                }
                g.Volume = Math.Clamp(g.Volume + diffPercent, 0, 200);
                this._holdUntil[$"gvol{idStr}"] = DateTime.UtcNow.AddMilliseconds(HoldMs);
                this.QueueSendLocked($"gvol{idStr}", "PATCH", $"/api/groups/{idStr}",
                    $"{{\"volume\":{Math.Round(g.Volume)}}}");
            }
        }

        public void ToggleGroupMute(String idStr)
        {
            lock (this._lock)
            {
                var g = this._groups.FirstOrDefault(x => x.Id.ToString() == idStr);
                if (g == null)
                {
                    return;
                }
                g.Muted = !g.Muted;
                this._holdUntil[$"gmute{idStr}"] = DateTime.UtcNow.AddMilliseconds(HoldMs);
                this.QueueSendLocked($"gmute{idStr}", "PATCH", $"/api/groups/{idStr}",
                    $"{{\"muted\":{(g.Muted ? "true" : "false")}}}");
            }
        }

        public void NudgeInputAzimuth(String idStr, Int32 deltaDegrees)
        {
            lock (this._lock)
            {
                var input = this._inputs.FirstOrDefault(x => x.Id.ToString() == idStr);
                if (input == null || !input.Spatial)
                {
                    return;
                }
                input.Azimuth = Math.Clamp(input.Azimuth + deltaDegrees, -180.0, 180.0);
                this._holdUntil[$"iaz{idStr}"] = DateTime.UtcNow.AddMilliseconds(HoldMs);
                var body = JsonSerializer.Serialize(new { azimuth = input.Azimuth, elevation = input.Elevation });
                this.QueueSendLocked($"iaz{idStr}", "POST", $"/api/inputs/{idStr}/direction", body);
            }
        }

        public void ToggleOutputMute(String name)
        {
            lock (this._lock)
            {
                var o = this._outputs.FirstOrDefault(x => x.Name == name);
                if (o == null)
                {
                    return;
                }
                o.Muted = !o.Muted;
                this._holdUntil[$"omute{name}"] = DateTime.UtcNow.AddMilliseconds(HoldMs);
                this.QueueSendLocked($"omute{name}", "POST", "/api/outputs/master",
                    JsonSerializer.Serialize(new { name, muted = o.Muted }));
            }
        }

        public void NudgeOutputVolume(String name, Int32 diffPercent)
        {
            lock (this._lock)
            {
                var o = this._outputs.FirstOrDefault(x => x.Name == name);
                if (o == null)
                {
                    return;
                }
                o.Master = Math.Clamp(o.Master + diffPercent, 0, 200);
                this._holdUntil[$"ovol{name}"] = DateTime.UtcNow.AddMilliseconds(HoldMs);
                this.QueueSendLocked($"ovol{name}", "POST", "/api/outputs/master",
                    JsonSerializer.Serialize(new { name, volume = Math.Round(o.Master) }));
            }
        }

        // Caller must hold _lock. Overwrites any pending send for the same key,
        // so only the latest value per target goes on the wire.
        private void QueueSendLocked(String key, String method, String path, String jsonBody)
        {
            this._pendingSends[key] = () => new HttpRequestMessage(new HttpMethod(method), this._baseUrl + path)
            {
                Content = new StringContent(jsonBody, Encoding.UTF8, "application/json"),
            };
        }

        private void SendLoop()
        {
            while (this._running)
            {
                KeyValuePair<String, Func<HttpRequestMessage>>[] batch;
                lock (this._lock)
                {
                    batch = this._pendingSends.ToArray();
                    this._pendingSends.Clear();
                }
                foreach (var kv in batch)
                {
                    try
                    {
                        using var response = this._http.Send(kv.Value());
                    }
                    catch
                    {
                        // Connection loss is reported by the poll loop; a lost
                        // write will be corrected by the next poll snapshot.
                    }
                }
                Thread.Sleep(SendIntervalMs);
            }
        }

        private void PollLoop()
        {
            var lastSignature = "";
            var pollCount = 0;
            while (this._running)
            {
                var ok = false;
                var changed = false;
                try
                {
                    var json = this._http.GetStringAsync(this._baseUrl + "/api/state").GetAwaiter().GetResult();
                    changed = this.ApplyState(json, ref lastSignature);
                    ok = true;
                    // Scenes change rarely (file adds/removes); refresh every ~10s.
                    if (pollCount++ % 10 == 0)
                    {
                        changed |= this.RefreshScenes();
                    }
                }
                catch
                {
                    // Mixer not running or mid-restart.
                }

                if (ok != this.Connected)
                {
                    this.Connected = ok;
                    this.ConnectionChanged?.Invoke(this, EventArgs.Empty);
                }
                if (changed)
                {
                    this.StateChanged?.Invoke(this, EventArgs.Empty);
                }
                Thread.Sleep(PollIntervalMs);
            }
        }

        private Boolean RefreshScenes()
        {
            var json = this._http.GetStringAsync(this._baseUrl + "/api/scenes").GetAwaiter().GetResult();
            using var doc = JsonDocument.Parse(json);
            var scenes = new List<String>();
            foreach (var s in doc.RootElement.GetProperty("scenes").EnumerateArray())
            {
                var name = s.GetProperty("name").GetString();
                if (!String.IsNullOrEmpty(name))
                {
                    scenes.Add(name);
                }
            }
            scenes.Sort(StringComparer.OrdinalIgnoreCase);
            lock (this._lock)
            {
                if (scenes.SequenceEqual(this._scenes))
                {
                    return false;
                }
                this._scenes = scenes;
            }
            return true;
        }

        private Boolean ApplyState(String json, ref String lastSignature)
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;

            var inputs = new List<MixerInput>();
            foreach (var i in root.GetProperty("inputs").EnumerateArray())
            {
                inputs.Add(new MixerInput
                {
                    Id = i.GetProperty("id").GetUInt32(),
                    Name = i.GetProperty("name").GetString() ?? "?",
                    Spatial = i.TryGetProperty("spatial", out var s) && s.GetBoolean(),
                    Azimuth = i.TryGetProperty("azimuth", out var a) ? a.GetDouble() : 0.0,
                    Elevation = i.TryGetProperty("elevation", out var e) ? e.GetDouble() : 0.0,
                });
            }

            var groups = new List<MixerGroup>();
            foreach (var g in root.GetProperty("groups").EnumerateArray())
            {
                groups.Add(new MixerGroup
                {
                    Id = g.GetProperty("id").GetUInt32(),
                    Name = g.GetProperty("name").GetString() ?? "?",
                    Volume = g.GetProperty("volume").GetDouble(),
                    Muted = g.GetProperty("muted").GetBoolean(),
                });
            }

            var outputs = new List<MixerOutput>();
            foreach (var o in root.GetProperty("outputs").EnumerateArray())
            {
                outputs.Add(new MixerOutput
                {
                    Name = o.GetProperty("name").GetString() ?? "?",
                    Master = o.GetProperty("master").GetDouble(),
                    Muted = o.TryGetProperty("muted", out var m) && m.GetBoolean(),
                });
            }

            lock (this._lock)
            {
                // Keep recently written values over (possibly stale) polled ones.
                var now = DateTime.UtcNow;
                foreach (var input in inputs)
                {
                    var old = this._inputs.FirstOrDefault(x => x.Id == input.Id);
                    if (old == null)
                    {
                        continue;
                    }
                    if (this._holdUntil.TryGetValue($"iaz{input.Id}", out var ta) && now < ta)
                    {
                        input.Azimuth = old.Azimuth;
                        input.Elevation = old.Elevation;
                    }
                }
                foreach (var g in groups)
                {
                    var old = this._groups.FirstOrDefault(x => x.Id == g.Id);
                    if (old == null)
                    {
                        continue;
                    }
                    if (this._holdUntil.TryGetValue($"gvol{g.Id}", out var tv) && now < tv)
                    {
                        g.Volume = old.Volume;
                    }
                    if (this._holdUntil.TryGetValue($"gmute{g.Id}", out var tm) && now < tm)
                    {
                        g.Muted = old.Muted;
                    }
                }
                foreach (var o in outputs)
                {
                    var old = this._outputs.FirstOrDefault(x => x.Name == o.Name);
                    if (old == null)
                    {
                        continue;
                    }
                    if (this._holdUntil.TryGetValue($"ovol{o.Name}", out var tv) && now < tv)
                    {
                        o.Master = old.Master;
                    }
                    if (this._holdUntil.TryGetValue($"omute{o.Name}", out var tm) && now < tm)
                    {
                        o.Muted = old.Muted;
                    }
                }

                this._inputs = inputs;
                this._groups = groups;
                this._outputs = outputs;
            }

            // Only report a change when something the device displays changed;
            // meter levels in the state json are deliberately not part of this.
            var signature = String.Join("|",
                inputs.Select(i => $"{i.Id}:{i.Name}:{i.Spatial}:{Math.Round(i.Azimuth)}")
                      .Concat(groups.Select(g => $"{g.Id}:{g.Name}:{Math.Round(g.Volume)}:{g.Muted}"))
                      .Concat(outputs.Select(o => $"{o.Name}:{Math.Round(o.Master)}:{o.Muted}")));
            if (signature == lastSignature)
            {
                return false;
            }
            lastSignature = signature;
            return true;
        }
    }
}
