using UnrealBuildTool;

public class SplineTools : ModuleRules
{
	public SplineTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"ProceduralMeshComponent"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"PhysicsCore"
			}
		);
	}
}
