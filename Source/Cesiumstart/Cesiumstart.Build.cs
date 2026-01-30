// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Cesiumstart : ModuleRules
{
	public Cesiumstart(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		
		// Use UE5.7 compatible warning setting
		UndefinedIdentifierWarningLevel = WarningLevel.Off;
	
		PublicDependencyModuleNames.AddRange(new string[] { 
			"Core", 
			"CoreUObject", 
			"Engine", 
			"InputCore",
			"Sockets",              // For UDP socket support
			"Networking",           // For FUdpSocketReceiver and related classes
			"ProceduralMeshComponent"  // For ShrinkWrap runtime mesh modification
		});
		
		// Add CesiumRuntime for Cesium actor access (not needed for vertex extraction!)
		// We use standard UE APIs - CesiumGltfPrimitiveComponent IS a UStaticMeshComponent
		// So we can access mesh data without private headers - no breaking risk!
		PrivateDependencyModuleNames.AddRange(new string[] {
			"CesiumRuntime"  // For CesiumGeoreference coordinate conversion
		});
	}
}
