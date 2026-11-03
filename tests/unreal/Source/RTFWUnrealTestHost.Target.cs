using UnrealBuildTool;

public class RTFWUnrealTestHostTarget : TargetRules
{
    public RTFWUnrealTestHostTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
        ExtraModuleNames.Add("RTFWUnrealTestHost");
    }
}
