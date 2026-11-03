using UnrealBuildTool;

public class RTFWUnrealTests : ModuleRules
{
    public RTFWUnrealTests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;
        bEnableExceptions = true;
        PrivateDependencyModuleNames.AddRange(
            new string[] { "Core", "CoreUObject", "Engine", "RTFWUnreal" });
        if (Target.bBuildEditor)
            PrivateDependencyModuleNames.Add("UnrealEd");
    }
}
