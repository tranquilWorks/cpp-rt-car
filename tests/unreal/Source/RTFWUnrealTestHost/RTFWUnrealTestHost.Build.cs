using UnrealBuildTool;

public class RTFWUnrealTestHost : ModuleRules
{
    public RTFWUnrealTestHost(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;
        bEnableExceptions = true;
        bUseRTTI = true;
        PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "RTFWUnreal" });
    }
}
