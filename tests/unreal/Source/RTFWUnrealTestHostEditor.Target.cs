using UnrealBuildTool;

public class RTFWUnrealTestHostEditorTarget : TargetRules
{
    public RTFWUnrealTestHostEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
        ExtraModuleNames.Add("RTFWUnrealTestHost");
    }
}
