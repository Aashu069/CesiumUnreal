#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Materials/MaterialInterface.h"
#include "CesiumTileMaterialSwapperComponent.generated.h"

/**
 * Replaces streamed Cesium tile materials based on their glTF base color.
 *
 * Intended for datasets where semantic parts are color-coded (e.g., wall=#FF00FF, window=#00FF00).
 * This component periodically scans mesh components belonging to a Cesium 3D Tileset actor and
 * swaps material slots whose base color matches configured palettes.
 */
UCLASS(ClassGroup = (Cesium), meta = (BlueprintSpawnableComponent))
class CESIUMSTART_API UCesiumTileMaterialSwapperComponent : public UActorComponent {
  GENERATED_BODY()

public:
  UCesiumTileMaterialSwapperComponent();

  /** Utility: convert #RRGGBB or #RRGGBBAA (sRGB) to linear. */
  static bool HexToLinearColor(const FString& Hex, FLinearColor& OutLinear);

  /** The Cesium 3D Tileset actor that owns streamed mesh components. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cesium|Material Swap")
  AActor* TilesetActor = nullptr;

  /** Materials to apply when a slot is classified as wall/roof/window. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cesium|Material Swap")
  UMaterialInterface* WallMaterial = nullptr;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cesium|Material Swap")
  UMaterialInterface* RoofMaterial = nullptr;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cesium|Material Swap")
  UMaterialInterface* WindowMaterial = nullptr;

  /**
   * If set, the swapper will override Cesium tile materials with this single material.
   * Use this when the dataset encodes semantic colors in vertex colors (glTF COLOR_0) rather than
   * via a glTF material baseColorFactor.
   */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cesium|Material Swap")
  UMaterialInterface* VertexColorClassifierMaterial = nullptr;

  /** How often to scan for newly streamed components. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cesium|Material Swap", meta = (ClampMin = "0.05", UIMin = "0.05"))
  float ScanIntervalSeconds = 0.5f;

  /** Color matching tolerance in linear space (0-1). */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cesium|Material Swap", meta = (ClampMin = "0.0", ClampMax = "1.0"))
  float ColorTolerance = 0.06f;

  /** If true, also attempts to classify by material name when color can't be read. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cesium|Material Swap")
  bool bFallbackToNameMatch = true;

  /** Enable log output to verify scanning and swapping in PIE. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cesium|Material Swap|Debug")
  bool bLogSwapper = false;

  /** Log discovered vector parameters (useful to find Cesium's base color parameter name). */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cesium|Material Swap|Debug")
  bool bLogMaterialParameters = false;

  /** Limit material-parameter logs per tick to avoid log spam. 0 = unlimited. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cesium|Material Swap|Debug", meta = (ClampMin = "0"))
  int32 MaxLoggedMaterialsPerTick = 10;

  /** Avoid repeatedly re-processing the same mesh components (recommended). */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cesium|Material Swap|Debug")
  bool bOnlyProcessEachComponentOnce = true;

  /**
   * Palettes (linear colors) that identify semantic parts.
   * Defaults match the provided SHADER_COLORS snippet.
   */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cesium|Material Swap|Palettes")
  TArray<FLinearColor> WallColors;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cesium|Material Swap|Palettes")
  TArray<FLinearColor> RoofColors;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cesium|Material Swap|Palettes")
  TArray<FLinearColor> WindowColors;

protected:
  virtual void BeginPlay() override;
  virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
  FTimerHandle ScanTimer;

  TSet<TWeakObjectPtr<UMeshComponent>> ProcessedComponents;

  int32 RemainingMaterialLogsThisTick = 0;

  void ScanAndSwap();

  static bool MeshHasVertexColors(UMeshComponent* MeshComponent);

  static bool TryGetBaseColorLinear(UMaterialInterface* Material, FLinearColor& OutBaseColor);
  static bool TryMatchAny(const FLinearColor& Color, const TArray<FLinearColor>& Palette, float Tolerance);

  void ProcessActor(AActor* ActorToProcess);
  void ProcessMeshComponent(UMeshComponent* MeshComponent);
  void ApplySwap(UMeshComponent* MeshComponent, int32 MaterialIndex, UMaterialInterface* Replacement);
};
