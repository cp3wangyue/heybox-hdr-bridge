# DisplayConfig.psm1 — 每显示器 HDR / SDR 白电平 / 色彩信息查询（P0 环境指纹与 AutoDetect 共用）
# 数据来源：user32 QueryDisplayConfig + DisplayConfigGetDeviceInfo（DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO / _SDR_WHITE_LEVEL）
# 注意：结构体封送全部在 C# 内完成。PowerShell 不能给嵌套结构体字段赋值（copy-back 陷阱），PS 端只读返回对象。
Set-StrictMode -Version 2

$cs = @"
using System;
using System.Runtime.InteropServices;

public static class DisplayConfigNative
{
    private const uint QDC_ONLY_ACTIVE_PATHS = 0x00000002;
    private const uint DCC_GET_SOURCE_NAME = 1;
    private const uint DCC_GET_SDR_WHITE_LEVEL = 11;
    private const uint DCC_GET_ADVANCED_COLOR_INFO = 12;

    private const uint AC_SUPPORTED = 0x1;
    private const uint AC_ENABLED = 0x2;
    private const uint WIDE_COLOR_ENFORCED = 0x4;

    [StructLayout(LayoutKind.Sequential)]
    private struct LUID { public uint LowPart; public int HighPart; }

    [StructLayout(LayoutKind.Sequential)]
    private struct RATIONAL { public uint Numerator; public uint Denominator; }

    [StructLayout(LayoutKind.Sequential)]
    private struct PATH_SOURCE_INFO {
        public LUID adapterId;
        public uint id;
        public uint modeInfoIdx;
        public uint statusFlags;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PATH_TARGET_INFO {
        public LUID adapterId;
        public uint id;
        public uint modeInfoIdx;
        public uint outputTechnology;
        public uint rotation;
        public uint scaling;
        public RATIONAL refreshRate;
        public uint scanLineOrdering;
        public int targetAvailable;
        public uint statusFlags;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PATH_INFO {
        public PATH_SOURCE_INFO sourceInfo;
        public PATH_TARGET_INFO targetInfo;
        public uint flags;
    }

    // sizeof == 64：union 用 6 个 ulong 占位（max 成员 = DISPLAYCONFIG_TARGET_MODE = 48 bytes）
    [StructLayout(LayoutKind.Sequential)]
    private struct MODE_INFO {
        public uint infoType;
        public uint id;
        public LUID adapterId;
        public ulong u0; public ulong u1; public ulong u2;
        public ulong u3; public ulong u4; public ulong u5;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DEVICE_INFO_HEADER {
        public uint type;
        public uint size;
        public LUID adapterId;
        public uint id;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct SDR_WHITE_LEVEL {
        public DEVICE_INFO_HEADER header;
        public uint SDRWhiteLevel;   // nits = SDRWhiteLevel * 80 / 1000
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ADVANCED_COLOR_INFO {
        public DEVICE_INFO_HEADER header;
        public uint value;
        public uint colorEncoding;
        public uint bitsPerColorChannel;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct SOURCE_DEVICE_NAME {
        public DEVICE_INFO_HEADER header;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string viewGdiDeviceName;
    }

    [DllImport("user32.dll")]
    private static extern int GetDisplayConfigBufferSizes(uint flags, ref uint numPathElements, ref uint numModeElements);

    [DllImport("user32.dll")]
    private static extern int QueryDisplayConfig(uint flags, ref uint numPathElements,
        [In, Out] PATH_INFO[] pathArray, ref uint numModeElements, [In, Out] MODE_INFO[] modeArray,
        IntPtr currentTopologyId);

    [DllImport("user32.dll")]
    private static extern int DisplayConfigGetDeviceInfo(ref SOURCE_DEVICE_NAME packet);

    [DllImport("user32.dll")]
    private static extern int DisplayConfigGetDeviceInfo(ref SDR_WHITE_LEVEL packet);

    [DllImport("user32.dll")]
    private static extern int DisplayConfigGetDeviceInfo(ref ADVANCED_COLOR_INFO packet);

    private static DEVICE_INFO_HEADER MakeHeader(uint type, uint size, LUID adapterId, uint id)
    {
        DEVICE_INFO_HEADER h;
        h.type = type;
        h.size = size;
        h.adapterId = adapterId;
        h.id = id;
        return h;
    }

    public struct MonitorHdrInfo
    {
        public string GdiDeviceName;
        public bool HdrEnabled;
        public bool HdrSupported;
        public bool WideColor;
        public uint BitsPerColor;
        public uint ColorEncoding;
        public double SdrWhiteNits;
        public double RefreshHz;
        public int SourceNameHr;
        public int SdrWhiteHr;
        public int AdvancedColorHr;
    }

    public static MonitorHdrInfo[] QueryAll()
    {
        uint numPath = 0, numMode = 0;
        int hr = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, ref numPath, ref numMode);
        if (hr != 0) throw new InvalidOperationException(string.Format("GetDisplayConfigBufferSizes failed hr=0x{0:X8}", hr));

        PATH_INFO[] paths = new PATH_INFO[numPath];
        MODE_INFO[] modes = new MODE_INFO[numMode];
        hr = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, ref numPath, paths, ref numMode, modes, IntPtr.Zero);
        if (hr != 0) throw new InvalidOperationException(string.Format("QueryDisplayConfig failed hr=0x{0:X8}", hr));

        MonitorHdrInfo[] result = new MonitorHdrInfo[numPath];
        for (int i = 0; i < numPath; i++)
        {
            PATH_INFO p = paths[i];
            MonitorHdrInfo m = new MonitorHdrInfo();

            SOURCE_DEVICE_NAME src = new SOURCE_DEVICE_NAME();
            src.header = MakeHeader(DCC_GET_SOURCE_NAME,
                (uint)Marshal.SizeOf(typeof(SOURCE_DEVICE_NAME)), p.sourceInfo.adapterId, p.sourceInfo.id);
            m.SourceNameHr = DisplayConfigGetDeviceInfo(ref src);
            m.GdiDeviceName = (m.SourceNameHr == 0) ? src.viewGdiDeviceName : string.Empty;

            SDR_WHITE_LEVEL sdr = new SDR_WHITE_LEVEL();
            sdr.header = MakeHeader(DCC_GET_SDR_WHITE_LEVEL,
                (uint)Marshal.SizeOf(typeof(SDR_WHITE_LEVEL)), p.targetInfo.adapterId, p.targetInfo.id);
            m.SdrWhiteHr = DisplayConfigGetDeviceInfo(ref sdr);
            m.SdrWhiteNits = (m.SdrWhiteHr == 0) ? Math.Round(sdr.SDRWhiteLevel * 80.0 / 1000.0, 1) : 0;

            ADVANCED_COLOR_INFO aci = new ADVANCED_COLOR_INFO();
            aci.header = MakeHeader(DCC_GET_ADVANCED_COLOR_INFO,
                (uint)Marshal.SizeOf(typeof(ADVANCED_COLOR_INFO)), p.targetInfo.adapterId, p.targetInfo.id);
            m.AdvancedColorHr = DisplayConfigGetDeviceInfo(ref aci);
            if (m.AdvancedColorHr == 0)
            {
                m.HdrSupported = (aci.value & AC_SUPPORTED) != 0;
                m.HdrEnabled = (aci.value & AC_ENABLED) != 0;
                m.WideColor = (aci.value & WIDE_COLOR_ENFORCED) != 0;
                m.BitsPerColor = aci.bitsPerColorChannel;
                m.ColorEncoding = aci.colorEncoding;
            }

            m.RefreshHz = (p.targetInfo.refreshRate.Denominator != 0)
                ? Math.Round((double)p.targetInfo.refreshRate.Numerator / p.targetInfo.refreshRate.Denominator, 2)
                : 0;

            result[i] = m;
        }
        return result;
    }
}
"@
if (-not ('DisplayConfigNative' -as [type])) {
    Add-Type -TypeDefinition $cs | Out-Null
}

function Get-DisplayHdrState {
    <#
    .SYNOPSIS
        返回每个活动显示器的 HDR 支持状态、SDR 参考白（nits）、每颜色位深与刷新率。
    #>
    $raw = [DisplayConfigNative]::QueryAll()
    $result = @()
    foreach ($m in $raw) {
        $result += [pscustomobject]@{
            GdiDeviceName = $m.GdiDeviceName
            HdrEnabled    = $m.HdrEnabled
            HdrSupported  = $m.HdrSupported
            WideColor     = $m.WideColor
            BitsPerColor  = $m.BitsPerColor
            ColorEncoding = $m.ColorEncoding
            SdrWhiteNits  = $m.SdrWhiteNits
            RefreshHz     = $m.RefreshHz
        }
    }
    return ,$result
}

Export-ModuleMember -Function Get-DisplayHdrState
