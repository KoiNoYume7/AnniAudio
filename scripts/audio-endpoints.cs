using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace AnniAudio
{
    public enum DataFlow
    {
        Render = 0,
        Capture = 1,
        All = 2
    }

    public enum Role
    {
        Console = 0,
        Multimedia = 1,
        Communications = 2
    }

    public class AudioSession
    {
        public int ProcessId { get; set; }
        public string ProcessName { get; set; }
        public string DisplayName { get; set; }
        public string DeviceId { get; set; }
        public bool IsSystemSounds { get; set; }
    }

    internal static class NativeMethods
    {
        [DllImport("ole32.dll")]
        internal static extern int CoInitializeEx(IntPtr pvReserved, int dwCoInit);
    }

    internal static class Combase
    {
        [DllImport("combase.dll", PreserveSig = false)]
        public static extern void RoGetActivationFactory(
            IntPtr activatableClassId,
            [In] ref Guid iid,
            out IntPtr factory);

        [DllImport("combase.dll", PreserveSig = false)]
        public static extern void WindowsCreateString(
            [MarshalAs(UnmanagedType.LPWStr)] string src,
            [In] uint length,
            [Out] out IntPtr hstring);

        [DllImport("combase.dll", PreserveSig = true)]
        public static extern int WindowsDeleteString(IntPtr hstring);
    }

    [ComImport, Guid("ab3d4648-e242-459f-b02f-541c70306324"), InterfaceType(ComInterfaceType.InterfaceIsIInspectable)]
    public interface IAudioPolicyConfigFactoryVariantFor21H2
    {
        int __incomplete__add_CtxVolumeChange();
        int __incomplete__remove_CtxVolumeChanged();
        int __incomplete__add_RingerVibrateStateChanged();
        int __incomplete__remove_RingerVibrateStateChange();
        int __incomplete__SetVolumeGroupGainForId();
        int __incomplete__GetVolumeGroupGainForId();
        int __incomplete__GetActiveVolumeGroupForEndpointId();
        int __incomplete__GetVolumeGroupsForEndpoint();
        int __incomplete__GetCurrentVolumeContext();
        int __incomplete__SetVolumeGroupMuteForId();
        int __incomplete__GetVolumeGroupMuteForId();
        int __incomplete__SetRingerVibrateState();
        int __incomplete__GetRingerVibrateState();
        int __incomplete__SetPreferredChatApplication();
        int __incomplete__ResetPreferredChatApplication();
        int __incomplete__GetPreferredChatApplication();
        int __incomplete__GetCurrentChatApplications();
        int __incomplete__add_ChatContextChanged();
        int __incomplete__remove_ChatContextChanged();

        [PreserveSig]
        uint SetPersistedDefaultAudioEndpoint(int processId, DataFlow flow, Role role, IntPtr deviceId);

        [PreserveSig]
        uint GetPersistedDefaultAudioEndpoint(int processId, DataFlow flow, Role role, [Out, MarshalAs(UnmanagedType.HString)] out string deviceId);

        [PreserveSig]
        uint ClearAllPersistedApplicationDefaultEndpoints();
    }

    [ComImport, Guid("2a59116d-6c4f-45e0-a74f-707e3fef9258"), InterfaceType(ComInterfaceType.InterfaceIsIInspectable)]
    public interface IAudioPolicyConfigFactoryVariantForDownlevel
    {
        int __incomplete__add_CtxVolumeChange();
        int __incomplete__remove_CtxVolumeChanged();
        int __incomplete__add_RingerVibrateStateChanged();
        int __incomplete__remove_RingerVibrateStateChange();
        int __incomplete__SetVolumeGroupGainForId();
        int __incomplete__GetVolumeGroupGainForId();
        int __incomplete__GetActiveVolumeGroupForEndpointId();
        int __incomplete__GetVolumeGroupsForEndpoint();
        int __incomplete__GetCurrentVolumeContext();
        int __incomplete__SetVolumeGroupMuteForId();
        int __incomplete__GetVolumeGroupMuteForId();
        int __incomplete__SetRingerVibrateState();
        int __incomplete__GetRingerVibrateState();
        int __incomplete__SetPreferredChatApplication();
        int __incomplete__ResetPreferredChatApplication();
        int __incomplete__GetPreferredChatApplication();
        int __incomplete__GetCurrentChatApplications();
        int __incomplete__add_ChatContextChanged();
        int __incomplete__remove_ChatContextChanged();

        [PreserveSig]
        uint SetPersistedDefaultAudioEndpoint(int processId, DataFlow flow, Role role, IntPtr deviceId);

        [PreserveSig]
        uint GetPersistedDefaultAudioEndpoint(int processId, DataFlow flow, Role role, [Out, MarshalAs(UnmanagedType.HString)] out string deviceId);

        [PreserveSig]
        uint ClearAllPersistedApplicationDefaultEndpoints();
    }

    public static class AudioPolicyHelper
    {
        private const int RPC_E_CHANGED_MODE = unchecked((int)0x80010106);
        private const int StubCount = 19;
        private static object _factoryLock = new object();
        private static bool _tried;
        private static IntPtr _policyPtr;
        private static IntPtr _vTable;
        public static string FactoryError { get; private set; }

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate uint SetPersistedDefaultAudioEndpointDelegate(IntPtr thisPtr, uint processId, DataFlow flow, Role role, IntPtr deviceId);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate uint GetPersistedDefaultAudioEndpointDelegate(IntPtr thisPtr, uint processId, DataFlow flow, Role role, out IntPtr deviceId);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate uint ClearAllPersistedApplicationDefaultEndpointsDelegate(IntPtr thisPtr);

        private static SetPersistedDefaultAudioEndpointDelegate _set;
        private static GetPersistedDefaultAudioEndpointDelegate _get;
        private static ClearAllPersistedApplicationDefaultEndpointsDelegate _clear;

        private static void EnsureFactory()
        {
            lock (_factoryLock)
            {
                if (_tried) return;
                _tried = true;

                int hr = NativeMethods.CoInitializeEx(IntPtr.Zero, 2 /* COINIT_APARTMENTTHREADED */);
                if (hr < 0 && hr != RPC_E_CHANGED_MODE) { FactoryError = "CoInitialize failed 0x" + hr.ToString("X8"); return; }

                IntPtr hClass = IntPtr.Zero;
                try
                {
                    Combase.WindowsCreateString("Windows.Media.Internal.AudioPolicyConfig", 40, out hClass);
                    if (TryCreatePolicy(hClass, typeof(IAudioPolicyConfigFactoryVariantFor21H2).GUID)) return;
                    if (TryCreatePolicy(hClass, typeof(IAudioPolicyConfigFactoryVariantForDownlevel).GUID)) return;
                    FactoryError = "AudioPolicyConfig factory not available";
                }
                catch (Exception ex) { FactoryError = ex.Message; }
                finally { if (hClass != IntPtr.Zero) Combase.WindowsDeleteString(hClass); }
            }
        }

        private static bool TryCreatePolicy(IntPtr hClass, Guid iid)
        {
            try
            {
                IntPtr ptr;
                Combase.RoGetActivationFactory(hClass, ref iid, out ptr);
                if (ptr == IntPtr.Zero) { FactoryError = "RoGetActivationFactory returned null"; return false; }
                _policyPtr = ptr;
                _vTable = Marshal.ReadIntPtr(_policyPtr);
                int setOffset = (7 + StubCount) * IntPtr.Size;
                int getOffset = (6 + StubCount) * IntPtr.Size;
                int clearOffset = (8 + StubCount) * IntPtr.Size;
                _set = Marshal.GetDelegateForFunctionPointer<SetPersistedDefaultAudioEndpointDelegate>(Marshal.ReadIntPtr(_vTable, setOffset));
                _get = Marshal.GetDelegateForFunctionPointer<GetPersistedDefaultAudioEndpointDelegate>(Marshal.ReadIntPtr(_vTable, getOffset));
                _clear = Marshal.GetDelegateForFunctionPointer<ClearAllPersistedApplicationDefaultEndpointsDelegate>(Marshal.ReadIntPtr(_vTable, clearOffset));
                return true;
            }
            catch (Exception ex) { FactoryError = ex.Message; return false; }
        }

        private static IntPtr StringToHString(string value)
        {
            IntPtr h;
            Combase.WindowsCreateString(value, (uint)value.Length, out h);
            return h;
        }

        public static string SetAppEndpoint(int processId, string deviceId)
        {
            EnsureFactory();
            if (_set == null) return "AudioPolicyConfig not available";

            IntPtr h = IntPtr.Zero;
            try
            {
                h = StringToHString(deviceId);
                foreach (Role role in new Role[] { Role.Console, Role.Multimedia, Role.Communications })
                {
                    uint hr = _set(_policyPtr, (uint)processId, DataFlow.Render, role, h);
                    if (hr != 0) return "0x" + hr.ToString("X8");
                }
                return "OK";
            }
            finally
            {
                if (h != IntPtr.Zero) Combase.WindowsDeleteString(h);
            }
        }

        public static string ClearAll()
        {
            EnsureFactory();
            if (_clear == null) return "AudioPolicyConfig not available";

            uint hr = _clear(_policyPtr);
            return (hr == 0) ? "OK" : "0x" + hr.ToString("X8");
        }
    }

    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
    public class MMDeviceEnumerator { }

    [ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMMDeviceCollection
    {
        [PreserveSig]
        int GetCount(out int count);

        [PreserveSig]
        int Item(int index, [MarshalAs(UnmanagedType.Interface)] out IMMDevice device);
    }

    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMMDevice
    {
        [PreserveSig]
        int Activate(ref Guid iid, int clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object ppInterface);

        [PreserveSig]
        int OpenPropertyStore(int stgmAccess, [MarshalAs(UnmanagedType.Interface)] out IPropertyStore properties);

        [PreserveSig]
        int GetId(out IntPtr ppstrId);

        [PreserveSig]
        int GetState(out int state);
    }

    [ComImport, Guid("886d8eeb-8cf2-4446-8d02-cdba1fbdcf99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IPropertyStore
    {
        // Not used; present so IMMDevice.OpenPropertyStore can be declared.
    }

    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMMDeviceEnumerator
    {
        [PreserveSig]
        int EnumAudioEndpoints(DataFlow dataFlow, int dwStateMask, [MarshalAs(UnmanagedType.Interface)] out IMMDeviceCollection ppDevices);

        [PreserveSig]
        int GetDefaultAudioEndpoint(DataFlow dataFlow, Role role, [MarshalAs(UnmanagedType.Interface)] out IMMDevice ppEndpoint);

        [PreserveSig]
        int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string pwstrId, [MarshalAs(UnmanagedType.Interface)] out IMMDevice ppDevice);
    }

    [ComImport, Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IAudioSessionManager2
    {
        [PreserveSig]
        int GetAudioSessionControl(ref Guid audioSessionGuid, int streamFlags, [MarshalAs(UnmanagedType.Interface)] out IAudioSessionControl sessionControl);

        [PreserveSig]
        int GetSimpleAudioVolume(ref Guid audioSessionGuid, int streamFlags, [MarshalAs(UnmanagedType.Interface)] out ISimpleAudioVolume audioVolume);

        [PreserveSig]
        int GetSessionEnumerator([MarshalAs(UnmanagedType.Interface)] out IAudioSessionEnumerator sessionEnum);

        [PreserveSig]
        int SetDuckingPreference(bool optOut);
    }

    [ComImport, Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IAudioSessionEnumerator
    {
        [PreserveSig]
        int GetCount(out int sessionCount);

        [PreserveSig]
        int GetSession(int sessionIndex, [MarshalAs(UnmanagedType.Interface)] out IAudioSessionControl session);
    }

    [ComImport, Guid("07379cef-7e49-4f10-a188-11f2b4b8b5ff"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IAudioSessionEvents { }

    [ComImport, Guid("f4b1a599-7266-4319-a8ca-e70acb11e8cd"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IAudioSessionControl
    {
        [PreserveSig]
        int GetState(out int state);

        [PreserveSig]
        int GetDisplayName(out IntPtr pRetVal);

        [PreserveSig]
        int SetDisplayName([MarshalAs(UnmanagedType.LPWStr)] string displayName, ref Guid eventContext);

        [PreserveSig]
        int GetIconPath(out IntPtr iconPath);

        [PreserveSig]
        int SetIconPath([MarshalAs(UnmanagedType.LPWStr)] string iconPath, ref Guid eventContext);

        [PreserveSig]
        int GetGroupingParam(out Guid groupingParam);

        [PreserveSig]
        int SetGroupingParam(ref Guid groupingParam, ref Guid eventContext);

        [PreserveSig]
        int GetLastActivation(out long lastActivation);

        [PreserveSig]
        int GetLastInactivation(out long lastInactivation);

        [PreserveSig]
        int RegisterAudioSessionNotification(IAudioSessionEvents client);

        [PreserveSig]
        int UnregisterAudioSessionNotification(IAudioSessionEvents client);
    }

    [ComImport, Guid("bfb7ff88-7239-4fc9-8fa2-07c950be9c6d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IAudioSessionControl2 : IAudioSessionControl
    {
        [PreserveSig]
        int GetSessionIdentifier(out IntPtr pRetVal);

        [PreserveSig]
        int GetSessionInstanceIdentifier(out IntPtr pRetVal);

        [PreserveSig]
        int GetProcessId(out int processId);

        [PreserveSig]
        int IsSystemSoundsSession();

        [PreserveSig]
        int SetDuckingPreference(bool optOut);
    }

    [ComImport, Guid("87CE5498-68D6-44E5-9215-6DA47EF883D8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface ISimpleAudioVolume
    {
        [PreserveSig]
        int SetMasterVolume(float level, ref Guid eventContext);

        [PreserveSig]
        int GetMasterVolume(out float level);

        [PreserveSig]
        int SetMute([MarshalAs(UnmanagedType.Bool)] bool mute, ref Guid eventContext);

        [PreserveSig]
        int GetMute([MarshalAs(UnmanagedType.Bool)] out bool mute);
    }

    [ComImport, Guid("5CDF2C82-841E-4546-9722-0CF74078229A"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IAudioEndpointVolume
    {
        int f();
        int g();
        int h();
        int i();

        [PreserveSig]
        int SetMasterVolumeLevelScalar(float fLevel, ref Guid pguidEventContext);

        int j();

        [PreserveSig]
        int GetMasterVolumeLevelScalar(out float pfLevel);

        int k();
        int l();
        int m();
        int n();

        [PreserveSig]
        int SetMute([MarshalAs(UnmanagedType.Bool)] bool bMute, ref Guid pguidEventContext);

        [PreserveSig]
        int GetMute([MarshalAs(UnmanagedType.Bool)] out bool pbMute);
    }

    public static class AudioEndpointHelper
    {
        private const int RPC_E_CHANGED_MODE = unchecked((int)0x80010106);
        private const int CLSCTX_ALL = 23;
        private const int DEVICE_STATE_ACTIVE = 0x00000001;
        private static bool _coInit;

        private static void CoInit()
        {
            if (_coInit) return;
            int hr = NativeMethods.CoInitializeEx(IntPtr.Zero, 2 /* COINIT_APARTMENTTHREADED */);
            if (hr < 0 && hr != RPC_E_CHANGED_MODE) Marshal.ThrowExceptionForHR(hr);
            _coInit = true;
        }

        private static string PtrToStringAndFree(IntPtr ptr)
        {
            if (ptr == IntPtr.Zero) return null;
            string s = Marshal.PtrToStringUni(ptr);
            Marshal.FreeCoTaskMem(ptr);
            return s;
        }

        private static IMMDevice GetDeviceById(string deviceId)
        {
            CoInit();
            IMMDeviceEnumerator enumerator = (IMMDeviceEnumerator)new MMDeviceEnumerator();
            IMMDevice device;
            int hr = enumerator.GetDevice(deviceId, out device);
            if (hr < 0) Marshal.ThrowExceptionForHR(hr);
            return device;
        }

        public static int GetEndpointVolume(string deviceId)
        {
            CoInit();
            IMMDevice device = GetDeviceById(deviceId);
            Guid iid = typeof(IAudioEndpointVolume).GUID;
            object obj;
            int hr = device.Activate(ref iid, CLSCTX_ALL, IntPtr.Zero, out obj);
            if (hr < 0) Marshal.ThrowExceptionForHR(hr);

            IAudioEndpointVolume volume = (IAudioEndpointVolume)obj;
            float level;
            hr = volume.GetMasterVolumeLevelScalar(out level);
            if (hr < 0) Marshal.ThrowExceptionForHR(hr);

            return (int)Math.Round(level * 100.0f);
        }

        public static void SetEndpointVolume(string deviceId, int percent)
        {
            CoInit();
            IMMDevice device = GetDeviceById(deviceId);
            Guid iid = typeof(IAudioEndpointVolume).GUID;
            object obj;
            int hr = device.Activate(ref iid, CLSCTX_ALL, IntPtr.Zero, out obj);
            if (hr < 0) Marshal.ThrowExceptionForHR(hr);

            IAudioEndpointVolume volume = (IAudioEndpointVolume)obj;
            float level = percent / 100.0f;
            if (level < 0.0f) level = 0.0f;
            if (level > 1.0f) level = 1.0f;
            Guid ctx = Guid.Empty;
            hr = volume.SetMasterVolumeLevelScalar(level, ref ctx);
            if (hr < 0) Marshal.ThrowExceptionForHR(hr);
        }

        public static List<AudioSession> GetAudioSessions()
        {
            CoInit();
            List<AudioSession> result = new List<AudioSession>();
            IMMDeviceEnumerator enumerator = (IMMDeviceEnumerator)new MMDeviceEnumerator();

            IMMDeviceCollection devices;
            int hr = enumerator.EnumAudioEndpoints(DataFlow.Render, DEVICE_STATE_ACTIVE, out devices);
            if (hr < 0) Marshal.ThrowExceptionForHR(hr);

            int deviceCount;
            devices.GetCount(out deviceCount);

            for (int d = 0; d < deviceCount; d++)
            {
                IMMDevice device;
                devices.Item(d, out device);
                if (device == null) continue;

                IntPtr idPtr;
                device.GetId(out idPtr);
                string deviceId = PtrToStringAndFree(idPtr);

                Guid iid = typeof(IAudioSessionManager2).GUID;
                object obj;
                hr = device.Activate(ref iid, CLSCTX_ALL, IntPtr.Zero, out obj);
                if (hr < 0) continue;

                IAudioSessionManager2 manager = (IAudioSessionManager2)obj;
                IAudioSessionEnumerator sessionEnum;
                hr = manager.GetSessionEnumerator(out sessionEnum);
                if (hr < 0) continue;

                int sessionCount;
                sessionEnum.GetCount(out sessionCount);
                for (int s = 0; s < sessionCount; s++)
                {
                    IAudioSessionControl ctrl;
                    sessionEnum.GetSession(s, out ctrl);
                    if (ctrl == null) continue;

                    IAudioSessionControl2 ctrl2 = ctrl as IAudioSessionControl2;
                    int pid = 0;
                    if (ctrl2 != null)
                    {
                        hr = ctrl2.GetProcessId(out pid);
                        if (hr < 0) pid = 0;
                    }

                    string displayName = null;
                    try
                    {
                        IntPtr dn;
                        hr = ctrl.GetDisplayName(out dn);
                        if (hr >= 0) displayName = PtrToStringAndFree(dn);
                    }
                    catch { }

                    string processName = null;
                    try
                    {
                        if (pid != 0)
                            processName = Process.GetProcessById(pid).ProcessName;
                    }
                    catch { }

                    bool isSystem = false;
                    if (ctrl2 != null)
                    {
                        try
                        {
                            hr = ctrl2.IsSystemSoundsSession();
                            if (hr == 0) isSystem = true;
                        }
                        catch { }
                    }

                    // Skip audio sessions that cannot be tied to a real process.
                    if (pid == 0 || string.IsNullOrEmpty(processName))
                    {
                        // Keep "System Sounds" visible so it can be routed too.
                        if (!isSystem) continue;
                        processName = "System Sounds";
                    }

                    result.Add(new AudioSession
                    {
                        ProcessId = pid,
                        ProcessName = processName,
                        DisplayName = displayName,
                        DeviceId = deviceId,
                        IsSystemSounds = isSystem
                    });
                }
            }

            return result;
        }
    }
}
