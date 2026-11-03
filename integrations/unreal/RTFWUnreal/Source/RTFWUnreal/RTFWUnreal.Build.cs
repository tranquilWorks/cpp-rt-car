using System;
using System.IO;
using UnrealBuildTool;

public class RTFWUnreal : ModuleRules
{
    public RTFWUnreal(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;
        bEnableExceptions = true;
        bUseRTTI = true;

        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

        string SdkRoot = Environment.GetEnvironmentVariable("RTFW_UNREAL_SDK_ROOT");
        if (String.IsNullOrWhiteSpace(SdkRoot))
        {
            throw new BuildException(
                "RTFW_UNREAL_SDK_ROOT must name the relocated exact-revision RTFW SDK built with the Unreal toolchain.");
        }

        string IncludeRoot = Path.Combine(SdkRoot, "include");
        string RuntimeArchive = Path.Combine(SdkRoot, "lib", "librtfw_runtime.a");
        if (!File.Exists(Path.Combine(IncludeRoot, "rt", "runtime.hpp")) ||
            !File.Exists(RuntimeArchive))
        {
            throw new BuildException(
                "RTFW_UNREAL_SDK_ROOT is missing include/rt/runtime.hpp or lib/librtfw_runtime.a.");
        }
        if (Target.Platform != UnrealTargetPlatform.Linux ||
            Target.Architectures.bIsMultiArch ||
            Target.Architecture != UnrealArch.X64)
        {
            throw new BuildException(
                "M19-02 is pinned only to the approved Debian 12 x86_64 Unreal 5.8.1 tuple.");
        }

        PublicSystemIncludePaths.Add(IncludeRoot);
        PublicAdditionalLibraries.Add(RuntimeArchive);
        string DataRoot = Environment.GetEnvironmentVariable("RTFW_UNREAL_DATA_ROOT");
        if (String.IsNullOrWhiteSpace(DataRoot))
            DataRoot = Path.Combine(SdkRoot, "share", "rtfw", "integrations");
        string LifecycleRoot = Path.Combine(DataRoot, "host_lifecycle");
        string PortableRoot = Path.Combine(DataRoot, "unreal", "portable");
        if (!File.Exists(Path.Combine(LifecycleRoot, "lifecycle.hpp")) ||
            !File.Exists(Path.Combine(PortableRoot, "rtfw_unreal", "worlds.hpp")))
            throw new BuildException("The exact SDK must include the host_lifecycle and unreal portable source kits. Set RTFW_UNREAL_DATA_ROOT for a custom data directory.");
        PublicSystemIncludePaths.Add(LifecycleRoot);
        PublicSystemIncludePaths.Add(PortableRoot);
    }
}
